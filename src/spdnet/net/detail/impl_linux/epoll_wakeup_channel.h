#ifndef SPDNET_NET_EPOLL_WAKEUP_CHANNEL_H_
#define SPDNET_NET_EPOLL_WAKEUP_CHANNEL_H_

#include <spdnet/base/noncopyable.h>
#include <spdnet/base/platform.h>
#include <spdnet/net/detail/impl_linux/epoll_channel.h>

namespace spdnet {
    namespace net {
        namespace detail {
            class epoll_wakeup_channel : public channel {
            public:
                explicit epoll_wakeup_channel() noexcept
                        : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

                ~epoll_wakeup_channel() {
                    socket_ops::close_socket(fd_);
                }

                void wakeup() {
                    int data = 1;
                    ::write(fd_, &data, sizeof(data));
                }

                sock_t eventfd() const { return fd_; }

            private:
                void on_send() override {

                }

                void on_recv() override {
                    char buf[1024]{0};
                    while (true) {
                        auto ret = ::read(fd_, buf, sizeof(buf));
                        if (ret == -1 || static_cast<size_t>(ret) < sizeof(buf))
                            break;
                    }
                }

                void on_close() override {

                }

            private:
                sock_t fd_;
            };

        }
    }

}

#endif  // SPDNET_NET_EPOLL_WAKEUP_CHANNEL_H_