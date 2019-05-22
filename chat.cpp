// clang++ -std=c++17 -stdlib=libc++ -Ifmt/include/ -Icppcoro/include -fcoroutines-ts chat.cpp fmt/src/format.cc -pthread -lboost_system

#include <algorithm>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio.hpp>
#include <boost/spirit/home/x3.hpp>
#include <cppcoro/task.hpp>
#include <experimental/coroutine>
#include <fmt/format.h>
#include <iostream>
#include <list>

namespace asio = boost::asio;
namespace ip   = asio::ip;
namespace x3   = boost::spirit::x3;
using tcp      = ip::tcp;

#define DEFINE_ASIO_COROUTINE(CLASS)                                           \
    template <typename... Args>                                                \
    struct std::experimental::coroutine_traits<void, CLASS&, Args...> {        \
        struct promise_type {                                                  \
            std::exception_ptr eptr;                                           \
                                                                               \
            suspend_never                                                      \
            initial_suspend () {                                               \
                return {};                                                     \
            }                                                                  \
                                                                               \
            suspend_never                                                      \
            final_suspend () {                                                 \
                return {};                                                     \
            }                                                                  \
                                                                               \
            void                                                               \
            return_void () {                                                   \
            }                                                                  \
                                                                               \
            void                                                               \
            get_return_object () {                                             \
            }                                                                  \
                                                                               \
            void                                                               \
            unhandled_exception () {                                           \
                eptr = std::current_exception ();                              \
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
        await_ready () {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return acceptor.async_accept (
                socket, [this, coro](boost::system::error_code ec) mutable {
                    if (ec) {
                        this->ec = ec;
                    }
                    coro.resume ();
                });
        }

        auto
        await_resume () {
            return ec;
        }
    };
    return Awaitable{acceptor, socket};
};

template <typename BufferSequence>
auto
async_write (tcp::socket& socket, BufferSequence const& buffers) {
    struct Awaitable {
        tcp::socket& socket;
        BufferSequence const& buffers;
        boost::system::error_code ec;

        bool
        await_ready () {
            return false;
        }

        void
        await_suspend (std::experimental::coroutine_handle<> coro) {
            return asio::async_write (
                socket,
                buffers,
                [this, coro](auto ec, std::size_t bytes_written) mutable {
                    if (ec) {
                        this->ec = ec;
                    }
                    coro.resume ();
                });
        }

        auto
        await_resume () {
            return ec;
        }
    };

    return Awaitable{socket, buffers};
};

template <typename Iterator>
struct contains_a_whole_line {
    using result_type = std::pair<Iterator, bool>;

    auto
    operator() (Iterator b, Iterator e) const {
        auto const crlf    = x3::char_ ("\r\n");
        auto const message = +(x3::char_ - crlf);
        auto const match   = *crlf >> message >> &(+x3::char_ ('\r') >> '\n');

        bool success = x3::parse (b, e, match);
        return result_type{b, success};
    }
};

template <typename DynamicBuffer>
inline auto
buffer_contains_a_whole_line (DynamicBuffer const& buffer) {
    using iterator =
        asio::buffers_iterator<typename DynamicBuffer::const_buffers_type>;
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
        await_ready () {
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
                    coro.resume ();
                });
        }

        auto
        await_resume () {
            if (!ec) {
                std::istream is (&buffer);
                do {
                    std::getline (is, line);
                    boost::algorithm::trim_if (
                        line, boost::algorithm::is_any_of ("\r "));
                } while (line.empty ());
            }
            return ec;
        }
    };
    return Awaitable{socket, buffer, line};
};

/*
 * Chat server
 */

class chat_user {
public:
    explicit chat_user (chat_server* const server, tcp::socket socket)
        : server_ (server), socket_ (std::move (socket)) {
    }

    cppcoro::task<> activate ();
    void deactivate ();
    bool is_active () const noexcept;
    void send (std::shared_ptr<std::string const> message);

private:
    chat_server* server_ = nullptr;
    tcp::socket socket_;
    std::string name_;
    bool active_ = false;
};

class chat_server {
public:
    explicit chat_server (asio::io_service& io)
        : acceptor_ (io, tcp::endpoint (ip::address::from_string ("127.0.0.1"),
                                        2222)) {
    }

    void activate ();
    void deactivate ();
    void broadcast (std::string const& sender, std::string message);

protected:
    void activate (std::list<chat_user>::iterator user);

private:
    tcp::acceptor acceptor_;
    std::list<chat_user> users_;
    bool active_ = false;
};

cppcoro::task<>
chat_user::activate () {
    if (active_) {
        co_return;
    }

    active_ = true;
    asio::streambuf buffer;
    std::string message;
    boost::system::error_code ec;

    do {
        ec = co_await async_read_line (socket_, buffer, message);

        if (ec == asio::error::operation_aborted) {
            continue;
        } else if (ec == asio::error::eof) {
            active_ = false;
            socket_.cancel (ec);
            server_->broadcast ("", fmt::format ("{} has disconnected", name_));
            co_return;
        } else if (ec) {
            throw boost::system::system_error (ec, ec.message ());
        }

        if (name_.empty ()) {
            name_ = std::move (message);
            server_->broadcast ("", fmt::format ("{} has joined", name_));
            continue;
        }

        if (message == ".shutdown") {
            server_->deactivate ();
            continue;
        }

        server_->broadcast (name_, std::move (message));
    } while (active_);

    socket_.shutdown (tcp::socket::shutdown_receive, ec);
}

void
chat_user::deactivate () {
    if (!active_) {
        return;
    }
    boost::system::error_code ec;
    active_ = false;
    socket_.shutdown (tcp::socket::shutdown_send, ec);
    socket_.cancel (ec);
}

bool
chat_user::is_active () const noexcept {
    return active_;
}

void
chat_user::send (std::shared_ptr<std::string const> message) {
    if (!active_) {
        co_return;
    }

    throw 42;

    auto ec = co_await async_write (socket_, asio::buffer (*message));
    if (ec && (ec != asio::error::operation_aborted)) {
        throw boost::system::system_error (ec, ec.message ());
    }
}

void
chat_server::activate (std::list<chat_user>::iterator user) {
    co_await user->activate ();
    users_.erase (user);
}

void
chat_server::activate () {
    if (active_) {
        co_return;
    }
    active_ = true;

    do {
        tcp::socket socket (acceptor_.get_io_service ());
        auto ec = co_await async_accept (acceptor_, socket);

        if (ec == asio::error::operation_aborted) {
            continue;
        } else if (ec) {
            throw boost::system::system_error (ec, ec.message ());
        }

        users_.emplace_back (this, std::move (socket));
        this->activate (std::prev (users_.end ()));
    } while (active_);
}

void
chat_server::deactivate () {
    if (!active_) {
        return;
    }
    boost::system::error_code ec;
    active_ = false;
    acceptor_.cancel (ec);

    for (auto& user : users_) {
        user.deactivate ();
    }
}

void
chat_server::broadcast (std::string const& sender, std::string message) {
    if (!active_) {
        return;
    }

    std::string msg;
    if (sender.empty ()) {
        msg = std::move (message);
        msg += '\n';
    } else {
        msg = sender + ": " + message + "\n";
    }
    auto shared_message = std::make_shared<std::string> (std::move (msg));
    for (auto& user : users_) {
        if (user.is_active ()) {
            user.send (shared_message);
        }
    }
}

int
main () {
    asio::io_service io;
    chat_server cs (io);

    cs.activate ();
    io.run ();
}
