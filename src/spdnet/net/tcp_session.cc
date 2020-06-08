#include <spdnet/net/tcp_session.h>
#include <cassert>
#include <iostream>
#include <string.h>
#include <spdnet/net/event_loop.h>

namespace spdnet {
    namespace net {
        TcpSession::TcpSession(std::shared_ptr<TcpSocket> socket, EventLoopPtr loop)
                : socket_(std::move(socket)),
                  loop_owner_(loop),
                  is_post_flush_(false),
                  is_can_write_(true) {

        }

        TcpSession::Ptr TcpSession::create(std::shared_ptr<TcpSocket> socket, EventLoopPtr loop) {
            return std::make_shared<TcpSession>(std::move(socket), loop);
        }

        void TcpSession::regWriteEvent() {
            struct epoll_event event{0, {nullptr}};
            event.events = EPOLLET | EPOLLIN | EPOLLOUT | EPOLLRDHUP;
            event.data.ptr = (Channel *) (this);
            ::epoll_ctl(loop_owner_->epoll_fd(), EPOLL_CTL_MOD, socket_->sock_fd(), &event);
        }

        void TcpSession::unregWriteEvent() {
            struct epoll_event event{0, {nullptr}};
            event.events = EPOLLET | EPOLLIN | EPOLLRDHUP;
            event.data.ptr = (Channel *) (this);
            ::epoll_ctl(loop_owner_->epoll_fd(), EPOLL_CTL_MOD, socket_->sock_fd(), &event);
        }

        void TcpSession::send(const char *data, size_t len) {
            if (len <= 0)
                return;
            auto buffer = loop_owner_->getBufferBySize(len);
            assert(buffer);
            buffer->write(data, len);
            {
                std::lock_guard<SpinLock> lck(send_guard_);
                send_buffer_list_.push_back(buffer);
            }
            if (is_post_flush_) {
                return;
            }
            is_post_flush_ = true;
            auto this_ptr = shared_from_this();
            loop_owner_->runInEventLoop([this_ptr]() {
                if (this_ptr->is_can_write_) {
                    this_ptr->flushBuffer();
                    this_ptr->is_post_flush_ = false;
                }
            });
        }

        void TcpSession::flushBuffer() {
            bool force_close = false;
            assert(loop_owner_->isInLoopThread());
            {
                std::lock_guard<SpinLock> lck(send_guard_);
                if (SPDNET_PREDICT_TRUE(pending_buffer_list_.empty())) {
                    pending_buffer_list_.swap(send_buffer_list_);
                } else {
                    for (const auto buffer : send_buffer_list_)
                        pending_buffer_list_.push_back(buffer);
                    send_buffer_list_.clear();
                }
            }

            constexpr size_t MAX_IOVEC = 1024;
            struct iovec iov[MAX_IOVEC];
            while (!pending_buffer_list_.empty()) {
                size_t cnt = 0;
                size_t prepare_send_len = 0;
                for (const auto buffer : pending_buffer_list_) {
                    iov[cnt].iov_base = buffer->getDataPtr();
                    iov[cnt].iov_len = buffer->getLength();
                    prepare_send_len += buffer->getLength();
                    cnt++;
                    if (cnt >= MAX_IOVEC)
                        break;
                }
                assert(cnt > 0);
                const int send_len = ::writev(socket_->sock_fd(), iov, static_cast<int>(cnt));
                if (SPDNET_PREDICT_FALSE(send_len < 0)) {
                    if (errno == EAGAIN) {
                        regWriteEvent();
                        is_can_write_ = false;
                    } else {
                        force_close = true;
                    }
                } else {
                    size_t tmp_len = send_len;
                    for (auto iter = pending_buffer_list_.begin(); iter != pending_buffer_list_.end();) {
                        auto buffer = *iter;
                        if (SPDNET_PREDICT_TRUE(buffer->getLength() <= tmp_len)) {
                            tmp_len -= buffer->getLength();
                            buffer->clear();
                            loop_owner_->releaseBuffer(buffer);
                            iter = pending_buffer_list_.erase(iter);
                        } else {
                            buffer->removeLength(tmp_len);
                            break;
                        }

                    }
                }
            }

            if (force_close) {
                onClose();
            }
        }

        void TcpSession::trySend() {
            is_can_write_ = true;
            unregWriteEvent();
            flushBuffer();
        }

        void TcpSession::tryRecv() {
            char stack_buffer[65536];
            bool force_close = false;
            while (true) {
                size_t valid_count = recv_buffer_.getWriteValidCount();
                struct iovec vec[2];
                vec[0].iov_base = recv_buffer_.getWritePtr();
                vec[0].iov_len = valid_count;
                vec[1].iov_base = stack_buffer;
                vec[1].iov_len = sizeof(stack_buffer);


                size_t try_recv_len = valid_count + sizeof(stack_buffer);

                int recv_len = static_cast<int>(::readv(socket_->sock_fd(), vec, 2));
                if (SPDNET_PREDICT_FALSE(recv_len == 0 || (recv_len < 0 && errno != EAGAIN))) {
                    force_close = true;
                    break;
                }
                size_t stack_len = 0;
                if (SPDNET_PREDICT_FALSE(recv_len > (int) valid_count)) {
                    recv_buffer_.addWritePos(valid_count);
                    stack_len = recv_len - valid_count;
                } else
                    recv_buffer_.addWritePos(recv_len);

                if (SPDNET_PREDICT_TRUE(nullptr != data_callback_)) {
                    size_t len = data_callback_(recv_buffer_.getDataPtr(), recv_buffer_.getLength());
                    assert(len <= recv_buffer_.getLength());
                    if (SPDNET_PREDICT_TRUE(len == recv_buffer_.getLength())) {
                        recv_buffer_.removeLength(len);
                        if (stack_len > 0) {
                            len = data_callback_(stack_buffer, stack_len);
                            assert(len <= stack_len);
                            if (len < stack_len) {
                                recv_buffer_.write(stack_buffer + len, stack_len - len);
                            } else if (len > stack_len) {
                                force_close = true;
                                break;
                            }
                        }
                    } else if (len < recv_buffer_.getLength()) {
                        recv_buffer_.removeLength(len);
                        if (stack_len > 0)
                            recv_buffer_.write(stack_buffer, stack_len);
                    } else {
                        force_close = true;
                        break;
                    }

                }

                if (SPDNET_PREDICT_FALSE(
                        recv_len >= (int) recv_buffer_.getCapacity()/* + (int)(sizeof(stack_buffer))*/)) {
                    size_t grow_len = 0;
                    if (recv_buffer_.getCapacity() * 2 <= max_recv_buffer_size_)
                        grow_len = recv_buffer_.getCapacity();
                    else
                        grow_len = max_recv_buffer_size_ - recv_buffer_.getCapacity();

                    if (grow_len > 0)
                        recv_buffer_.grow(grow_len);
                }

                if (SPDNET_PREDICT_FALSE(recv_buffer_.getWriteValidCount() == 0 || recv_buffer_.getLength() == 0))
                    recv_buffer_.adjustToHead();

                if (recv_len < static_cast<int>(try_recv_len))
                    break;
            }

            if (force_close)
                onClose();
        }

        void TcpSession::onClose() {
            if (has_closed)
                return;
            assert(loop_owner_->isInLoopThread());
            has_closed = true;
            auto loop = loop_owner_;
            auto callback = disconnect_callback_;
            Ptr connection = shared_from_this();
            std::shared_ptr<TcpSocket> socket = std::move(socket_);
            loop_owner_->runAfterEventLoop([loop, callback, connection, socket]() {
                if (callback)
                    callback(connection);
                loop->removeTcpSession(socket->sock_fd());
                struct epoll_event ev{0, {nullptr}};
                ::epoll_ctl(loop->epoll_fd(), EPOLL_CTL_DEL, socket->sock_fd(), &ev);
            });
            is_can_write_ = false;
            disconnect_callback_ = nullptr;
            data_callback_ = nullptr;
            socket_.reset();
        }

        void TcpSession::postDisconnect() {
            auto loop = loop_owner_;
            auto this_ptr = shared_from_this();
            loop_owner_->runInEventLoop([loop, this_ptr]() {
                this_ptr->onClose();
            });
        }

        void TcpSession::postShutDown() {
            auto loop = loop_owner_;
            auto this_ptr = shared_from_this();
            loop_owner_->runInEventLoop([loop, this_ptr]() {
                loop->runAfterEventLoop([this_ptr]() {
                    this_ptr->execShutDownInLoop();
                });
            });
        }

        void TcpSession::execShutDownInLoop() {
            assert(socket_ != nullptr);
            assert(loop_owner_->isInLoopThread());
            if (socket_ != nullptr) {
                ::shutdown(socket_->sock_fd(), SHUT_WR);
            }
            is_can_write_ = false;
        }

        void TcpSession::setDisconnectCallback(TcpDisconnectCallback &&callback) {
            disconnect_callback_ = std::move(callback);
        }

        void TcpSession::setDataCallback(TcpDataCallback &&callback) {
            data_callback_ = std::move(callback);
        }

    }
}