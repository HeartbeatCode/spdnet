#ifndef SPDNET_NET_DETAIL_IMPL_WIN_IOCP_RECV_CHANNEL_H_
#define SPDNET_NET_DETAIL_IMPL_WIN_IOCP_RECV_CHANNEL_H_

#include <system_error>
#include <spdnet/base/noncopyable.h>
#include <spdnet/base/platform.h>
#include <iostream>
#include <spdnet/net/detail/impl_win/iocp_impl.h>
#include <spdnet/net/detail/impl_win/iocp_channel.h>

namespace spdnet {
    namespace net {
        namespace detail {
            class iocp_recv_channel : public socket_channel {
            public:
                iocp_recv_channel(socket_data::ptr data, std::shared_ptr<iocp_impl> impl)
                        : socket_channel(data, impl) {

                }

                void start_recv() {
                    buf_.len = data_->recv_buffer_.get_write_valid_count();
                    buf_.buf = data_->recv_buffer_.get_write_ptr();

                    DWORD bytes_transferred = 0;
                    DWORD recv_flags = 0;
                    reset();
                    int result = ::WSARecv(data_->sock_fd(), &buf_, 1, &bytes_transferred, &recv_flags, (LPOVERLAPPED)
                    this, 0);
                    DWORD last_error = ::WSAGetLastError();
                    if (result != 0 && last_error != WSA_IO_PENDING) {
                        io_impl_->close_socket(data_);
                    }
                }

            private:
                void do_complete(size_t bytes_transferred, std::error_code ec) override {
                    bool force_close = false;
                    if (bytes_transferred == 0 || ec) {
                        // eof 
                        force_close = true;
                    } else {
                        auto &recv_buffer = data_->recv_buffer_;
                        auto post_len = recv_buffer.get_write_valid_count();
                        recv_buffer.add_write_pos(bytes_transferred);
                        if (nullptr != data_->data_callback_) {
                            size_t len = data_->data_callback_(recv_buffer.get_data_ptr(), recv_buffer.get_length());
                            assert(len <= recv_buffer.get_length());
                            if (len <= recv_buffer.get_length()) {
                                recv_buffer.remove_length(len);
                            } else {
                                force_close = true;
                            }
                        }

                        if (post_len == bytes_transferred) {
                            size_t grow_len = 0;
                            if (recv_buffer.get_capacity() * 2 <= data_->max_recv_buffer_size_)
                                grow_len = recv_buffer.get_capacity();
                            else
                                grow_len = data_->max_recv_buffer_size_ - recv_buffer.get_capacity();

                            if (grow_len > 0)
                                recv_buffer.grow(grow_len);
                        }

                        if (SPDNET_PREDICT_FALSE(
                                recv_buffer.get_write_valid_count() == 0 || recv_buffer.get_length() == 0))
                            recv_buffer.adjust_to_head();
                    }


                    if (force_close)
                        io_impl_->close_socket(data_);
                    else
                        this->start_recv();
                }

            private:
                WSABUF buf_ = {0, 0};
            };
        }
    }

}

#endif  // SPDNET_NET_DETAIL_IMPL_WIN_IOCP_RECV_CHANNEL_H_