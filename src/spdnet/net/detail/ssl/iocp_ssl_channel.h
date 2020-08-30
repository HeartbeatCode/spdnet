#ifndef SPDNET_NET_DETAIL_IMPL_WIN_IOCP_SSL_CHANNEL_H_
#define SPDNET_NET_DETAIL_IMPL_WIN_IOCP_SSL_CHANNEL_H_

#include <system_error>
#include <spdnet/base/noncopyable.h>
#include <spdnet/base/platform.h>
#include <spdnet/net/detail/impl_win/iocp_impl.h>
#include <spdnet/net/detail/impl_win/iocp_channel.h>
#include <spdnet/base/buffer_pool.h>
#include <spdnet/net/tcp_session.h>

namespace spdnet {
    namespace net {
        namespace detail {
            class iocp_ssl_send_channel : public socket_channel {
            public:
                iocp_ssl_send_channel(std::shared_ptr<tcp_session> session, std::shared_ptr<io_impl_type> io_impl)
                        : socket_channel(session, io_impl) {

                }

                void flush_buffer() {
                    if (!session_->ssl_context_->has_already_handshake()) {
                        session_->ssl_context_->try_start_ssl_handshake(); 
                        if (!session_->ssl_context_->has_already_handshake()) {
                            post_write_event();
                            return;
                        }
                    }
                    {
                        std::lock_guard<spdnet::base::spin_lock> lck(session_->send_guard_);
                        if (SPDNET_PREDICT_TRUE(session_->pending_packet_list_.empty())) {
                            session_->pending_packet_list_.swap(session_->send_packet_list_);
                        } else {
                            for (const auto& packet : session_->send_packet_list_)
                                session_->pending_packet_list_.push_back(packet);
                            session_->send_packet_list_.clear();
                        }
                    }
                    for (const auto& packet : session_->pending_packet_list_) {
                        buffer_.write(packet.buffer_->get_data_ptr(), packet.buffer_->get_length()); 
                    }
                    bool force_close = false; 
                    int send_len = SSL_write(session_->ssl_context_->get_ssl(), buffer_.get_data_ptr(), buffer_.get_length());
                    if (send_len <= 0) {
						if ((SSL_get_error(session_->ssl_context_->get_ssl(), send_len) != SSL_ERROR_WANT_WRITE) &&
							(current_errno() != WSAEWOULDBLOCK))
						{
                            force_close = true; 
						}
                    }
                    else {
						for (auto iter = session_->pending_packet_list_.begin();
							iter != session_->pending_packet_list_.end();) {
							auto& packet = *iter;
							if (SPDNET_PREDICT_TRUE(packet.buffer_->get_length() <= send_len)) {
								send_len -= packet.buffer_->get_length();
								packet.buffer_->clear();
								io_impl_->recycle_buffer(packet.buffer_);
								if (packet.callback_)
									packet.callback_();
								iter = session_->pending_packet_list_.erase(iter);
							}
							else {
								packet.buffer_->remove_length(send_len);
								break;
							}

						}
                    }

                    buffer_.clear();
                    if (force_close) {
                        io_impl_->close_socket(session_);
                    }
                    else if (!session_->pending_packet_list_.empty()){
                        post_write_event(); 
                    }
                    else {
                        session_->is_post_flush_ = false; 
                        if (!session_->send_packet_list_.empty()) {
                            post_write_event();
                        }
                    }
                }

            private:
                void post_write_event()
                {
					static CHAR send_tmp_data_[] = { 0 };
					static WSABUF send_buf_ = { 0, send_tmp_data_ };


					DWORD send_len = 0;
					const int result = ::WSASend(session_->sock_fd() , 
                        &send_buf_ , 
						1, 
						&send_len,
						0,
						(LPOVERLAPPED)this,
						0);
					DWORD last_error = current_errno();
					if (result != 0 && last_error != WSA_IO_PENDING) {
						io_impl_->close_socket(session_);
					}
                }
                void do_complete(size_t bytes_transferred, std::error_code ec) override {
                    if (/*bytes_transferred == 0 || */ec) {
                        io_impl_->close_socket(session_);
                    } else {
                        flush_buffer();
                    }
                }
                spdnet::base::buffer buffer_; 
            };

            class iocp_ssl_recv_channel : public socket_channel {
            public:
                iocp_ssl_recv_channel(std::shared_ptr<tcp_session> session, std::shared_ptr<io_impl_type> io_impl)
                    : socket_channel(session, io_impl) {

                }

                void start_recv() {
					static CHAR recv_tmp_data_[] = { 0 };
					static WSABUF recv_buf_ = { 0, recv_tmp_data_ };

					DWORD bytes_transferred = 0;
					DWORD recv_flags = 0;
					reset();
					int result = ::WSARecv(session_->sock_fd(), &recv_buf_, 1, &bytes_transferred, &recv_flags, (LPOVERLAPPED)
						this, 0);
					DWORD last_error = ::WSAGetLastError();
					if (result != 0 && last_error != WSA_IO_PENDING) {
						io_impl_->close_socket(session_);
					}
                }

            private:
                void do_complete(size_t bytes_transferred, std::error_code ec) override {
                    assert(session_->ssl_context_ != nullptr);
                    if (/*bytes_transferred == 0 || */ec) {
                        io_impl_->close_socket(session_);
                    }
                    else {
                        if (!session_->ssl_context_->has_already_handshake()) {
                            session_->ssl_context_->try_start_ssl_handshake();
							if (!session_->ssl_context_->has_already_handshake()) {
                                start_recv();
                                return; 
							}
                        }
                        auto& recv_buffer = session_->recv_buffer_;
                        bool force_close = false; 
                        while (true) {
                            size_t write_valid_count = recv_buffer.get_write_valid_count(); 
                            int ret_len = SSL_read(session_->ssl_context_->get_ssl(),
                                recv_buffer.get_write_ptr(), write_valid_count);
                            if (ret_len == 0) {
                                force_close = true; 
                                break; 
                            }
                            else if (ret_len < 0) {
								if ((SSL_get_error(session_->ssl_context_->get_ssl(), ret_len) != SSL_ERROR_WANT_READ) &&
									(current_errno() != WSAEWOULDBLOCK))
								{
                                    force_close = true;
								}
                                break;
                            }
                            else {
                                recv_buffer.add_write_pos(ret_len);
                                if (!session_->exec_data_callback(recv_buffer)) {
                                    force_close = true;
                                }
                                recv_buffer.try_adjust_to_head(); 
								if (write_valid_count == ret_len) {
                                    recv_buffer.adjust_capacity(session_->max_recv_buffer_size_); 
								}
                                else {
                                    break; 
                                }
                            }
                        }

                        if (force_close) {
                            io_impl_->close_socket(session_);
                        }
                        else {
                            this->start_recv();
                        }
                    }
                }
            };
        }
    }

}

#endif  // SPDNET_NET_DETAIL_IMPL_WIN_IOCP_SSL_CHANNEL_H_