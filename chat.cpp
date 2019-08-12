// clang++ -std=c++17 -stdlib=libc++ -Ifmt/include/ -Icppcoro/include -fcoroutines-ts -fvisibility=hidden -flto -O3 -DNDEBUG -DBOOST_EXCEPTION_DISABLE chat.cpp fmt/src/format.cc -pthread -Wl,-Bstatic -lboost_system -Wl,-Bdynamic

#include <algorithm>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio.hpp>
#include <boost/spirit/home/x3.hpp>
#include <chrono>
#include <cppcoro/task.hpp>
#include <experimental/coroutine>
#include <fmt/format.h>
#include <iostream>
#include <list>

namespace asio = boost::asio;
namespace ip   = asio::ip;
namespace x3   = boost::spirit::x3;
using tcp      = ip::tcp;

/* TODO: how to get access to CLASS */

#define DEFINE_ASIO_COROUTINE(CLASS)                                           \
    template <typename... Args>                                                \
    struct std::experimental::coroutine_traits<void, CLASS&, Args...> {        \
        struct promise_type {                                                  \
            std::exception_ptr eptr;                                           \
                                                                               \
            suspend_never                                                      \
            initial_suspend() {                                                \
                return {};                                                     \
            }                                                                  \
                                                                               \
            suspend_never                                                      \
            final_suspend() {                                                  \
                return {};                                                     \
            }                                                                  \
                                                                               \
            void                                                               \
            return_void() {                                                    \
            }                                                                  \
                                                                               \
            void                                                               \
            get_return_object() {                                              \
            }                                                                  \
                                                                               \
            void                                                               \
            unhandled_exception() {                                            \
                eptr = std::current_exception();                               \
            }                                                                  \
        };                                                                     \
    };

class chat_server;
class chat_user;

DEFINE_ASIO_COROUTINE (chat_server)
DEFINE_ASIO_COROUTINE (chat_user)

auto
async_accept (tcp::acceptor& acceptor, tcp::socket& socket) {
    struct Awaitable {
        tcp::acceptor& acceptor;
        tcp::socket& socket;
        boost::system::error_code ec;

        bool
        await_ready() {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return acceptor.async_accept (
                socket, [this, coro](boost::system::error_code ec) mutable {
                    if (ec) {
                        this->ec = ec;
                    }
                    coro.resume();
                }
            );
        }

        auto
        await_resume() {
            return ec;
        }
    };
    return Awaitable{acceptor, socket};
};

template <typename BufferSequence>
auto
async_write (tcp::socket& socket, BufferSequence const& buffers,
             std::size_t* bytes_written = nullptr) {
    struct Awaitable {
        tcp::socket& socket;
        BufferSequence const& buffers;
        std::size_t* bytes_written = nullptr;
        boost::system::error_code ec;

        bool
        await_ready() {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return asio::async_write (
                socket, buffers,
                [this, coro](auto const ec, std::size_t const bytes) mutable {
                    if (ec) {
                        this->ec = ec;
                    }
                    if (this->bytes_written) {
                        *this->bytes_written = bytes;
                    }
                    coro.resume();
                }
            );
        }

        auto
        await_resume() {
            return ec;
        }
    };

    return Awaitable{socket, buffers, bytes_written};
};

template <typename Iterator>
struct contains_a_whole_line {
    using result_type = std::pair<Iterator, bool>;

    auto
    operator() (Iterator b, Iterator e) const {
        auto const crlf    = x3::char_ ("\r\n");
        auto const ws      = x3::char_ ("\r\n\t ");
        auto const message = +(~crlf);
        auto const match   = *ws >> message >> &(+x3::char_ ('\r') >> '\n');

        bool success = x3::parse (b, e, match);
        return result_type{b, success};
    }
};

template <typename DynamicBuffer> inline
auto
buffer_contains_a_whole_line (DynamicBuffer const& buffer) {
    using iterator = asio::buffers_iterator<typename DynamicBuffer::const_buffers_type>;
    return contains_a_whole_line<iterator>{};
}

auto
async_read_line (tcp::socket& socket, asio::streambuf& buffer,
                 std::string& line) {
    struct Awaitable {
        tcp::socket& socket;
        asio::streambuf& buffer;
        std::string& line;
        boost::system::error_code ec;

        bool
        await_ready() {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return asio::async_read_until (
                socket,
                buffer,
                buffer_contains_a_whole_line (buffer),
                [this, coro](auto ec, std::size_t bytes_read) mutable {
                    if (ec) {
                        this->ec = ec;
                    }
                    coro.resume();
                });
        }

        auto
        await_resume() {
            std::istream is (&buffer);
            do {
                std::getline (is, line);
                boost::algorithm::trim_if (line, boost::algorithm::is_any_of ("\t\r "));
            } while (line.empty() && is.good());
            return ec;
        }
    };
    return Awaitable{socket, buffer, line};
};

auto
async_wait (asio::steady_timer& timer) {
    struct Awaitable {
        asio::steady_timer& timer;
        boost::system::error_code ec;

        bool
        await_ready() {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return timer.async_wait ([this, coro](auto ec) mutable {
                if (ec) {
                    this->ec = ec;
                }
                coro.resume();
            });
        }

        auto
        await_resume() {
            return ec;
        }
    };
    return Awaitable{timer};
}

/*
 * Chat server
 */
class chat_user final {
public:
    explicit
    chat_user (chat_server* const server, int id,
               tcp::socket socket):
        server_ (server),
        id_ (id),
        socket_ (std::move (socket)),
        timer_ (socket_.get_io_service()) {
    }

    cppcoro::task<>
    async_activate();

    void
    async_deactivate();

    template <typename Duration = std::chrono::seconds>
    void
    async_disconnect_after (Duration&&);

    bool
    is_active() const noexcept;

    int
    id() const noexcept {
        return id_;
    }

    std::string
    name() const {
        return name_;
    }

    void
    async_send (std::shared_ptr<std::string const> message);

private:
    chat_server* server_ = nullptr;
    int id_ = 0;
    std::string name_ = "[anonymous]";
    tcp::socket socket_;
    asio::steady_timer timer_;
    bool active_ = false;
};

class chat_server final {
public:
    explicit
    chat_server (asio::io_service& io)
        : acceptor_ (io, tcp::endpoint (ip::address::from_string ("127.0.0.1"),
                                        2222)) {
    }

    void async_activate();
    void async_deactivate();

    int
    add_user (tcp::socket socket);

    cppcoro::task<void>
    broadcast (std::string message);

    cppcoro::task<void>
    broadcast (chat_user const& sender, std::string const& message);

protected:
    void
    async_activate (std::list<chat_user>::iterator user);

private:
    tcp::acceptor acceptor_;
    std::list<chat_user> users_;
    int next_user_id_ = 1;
    bool active_ = false;
};

template <typename Duration> inline
void
chat_user::async_disconnect_after (Duration&& interval) {
    boost::system::error_code ec;
    timer_.expires_from_now (std::forward<Duration>(interval), ec);

    ec = co_await async_wait (timer_);
    if (ec == asio::error::operation_aborted) {
        co_return;
    } else if (ec) {
        std::cout << fmt::format ("Timeout on user socket failed: {} - {}\n",
                                  ec.value(), ec.message());
    }

    socket_.close (ec);
    if (ec) {
        std::cout << fmt::format ("Close on user socket failed: {} - {}\n",
                                  ec.value(), ec.message());
    }
}

cppcoro::task<>
chat_user::async_activate() {
    if (active_) {
        co_return;
    }

    active_ = true;
    asio::streambuf buffer (8192);
    std::string message;
    boost::system::error_code ec;

    do {
        ec = co_await async_read_line (socket_, buffer, message);

        if (ec == asio::error::operation_aborted) {
            continue;
        } else if (ec == asio::error::eof) {
            active_ = false;
            timer_.cancel (ec);
            socket_.close (ec);
            co_await server_->broadcast (fmt::format ("{} has disconnected\n",
                                         name_));
            co_return;
        } else if (ec) {
            std::cout << fmt::format ("Read from user socket failed: {} - {}\n",
                                      ec.value(), ec.message());
            break;
        }

        /* Cancel the timeout here, because broadcast ops can take a long
           time */
        timer_.cancel (ec);

        if (message == ".shutdown") {
            server_->async_deactivate();
            continue;
        } else if (name_.empty()) {
            name_ = std::move (message);
            co_await server_->broadcast (fmt::format ("{} has joined\n", name_));
        } else {
            co_await server_->broadcast (*this, std::move (message));
        }

        this->async_disconnect_after (std::chrono::minutes (3));
    } while (true);
}

void
chat_user::async_deactivate() {
    static auto const goodbye = std::make_shared<std::string> ("Goodbye!\n");
    if (!active_) {
        co_return;
    }

    this->async_send (goodbye);
    active_ = false;
    boost::system::error_code ec;
    socket_.shutdown (tcp::socket::shutdown_send, ec);
    if (ec) {
        std::cout << fmt::format("User socket shutdown failed: {} - {}\n",
                                  ec.value(), ec.message());
        if (ec == boost::system::errc::bad_file_descriptor) {
            co_return;
        }
    }

    this->async_disconnect_after (std::chrono::seconds (5));
}

bool
chat_user::is_active() const noexcept {
    return active_;
}

void
chat_user::async_send (std::shared_ptr<std::string const> message) {
    if (!active_) {
        co_return;
    }

    auto ec = co_await async_write (socket_, asio::buffer (*message));
    if (ec) {
        std::cout << fmt::format ("Write to user socket failed: {} - {}\n",
                                  ec.value(), ec.message());
    }
}

void
chat_server::async_activate (std::list<chat_user>::iterator user) {
    co_await user->async_activate();
    std::cout << fmt::format ("Deleting user {} - {}\n", user->id(), user->name());
    users_.erase (user);
}

int
chat_server::add_user (tcp::socket socket) {
    auto const new_user_id = next_user_id_++;
    users_.emplace_back (this, new_user_id, std::move (socket));
    auto const user = std::prev (users_.end());
    user->async_disconnect_after (std::chrono::seconds (10));
    this->async_activate (user);
    return new_user_id;
}

void
chat_server::async_activate() {
    if (active_) {
        co_return;
    }
    active_ = true;

    do {
        tcp::socket socket (acceptor_.get_io_service());
        auto ec = co_await async_accept (acceptor_, socket);

        if (ec == asio::error::operation_aborted) {
            continue;
        } else if (ec) {
            std::cout << fmt::format ("Server accept failed: {} - {}\n",
                                      ec.value(), ec.message());
            continue;
        }

        socket.set_option (tcp::socket::linger (true, 0), ec);
        this->add_user (std::move (socket));
    } while (active_);
}

void
chat_server::async_deactivate() {
    if (!active_) {
        return;
    }
    active_ = false;
    boost::system::error_code ec;
    acceptor_.cancel (ec);

    for (auto& user: users_) {
        user.async_deactivate();
    }
}

cppcoro::task<void>
chat_server::broadcast (std::string message) {
    if (!active_) {
        co_return;
    }

    auto shared_message = std::make_shared<std::string> (std::move (message));
    for (auto& user: users_) {
        /* This check is an optimization: we don't want to spawn coroutines
           for inactive users */
        if (user.is_active()) {
            user.async_send (shared_message);
        }
    }
}

cppcoro::task<void>
chat_server::broadcast
(chat_user const& sender, std::string const& message) {
    return this->broadcast (fmt::format ("{}: {}\n", sender.name(), message));
}

int
main() {
    asio::io_service io;
    chat_server cs (io);
    cs.async_activate();
    io.run();
}
