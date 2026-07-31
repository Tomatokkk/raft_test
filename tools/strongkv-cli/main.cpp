#include <strongkv/client.h>

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split_command(const std::string& line) {
    std::vector<std::string> output;
    std::string current;
    char quote = '\0';
    bool escaped = false;
    for (const char c : line) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (std::isspace(
                       static_cast<unsigned char>(c)) != 0) {
            if (!current.empty()) {
                output.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (escaped || quote != '\0') {
        throw std::runtime_error("unfinished escape or quote");
    }
    if (!current.empty()) {
        output.push_back(std::move(current));
    }
    return output;
}

void print_value(const strongkv::ClientValue& value,
                 std::size_t indent = 0) {
    const std::string prefix(indent, ' ');
    switch (value.type) {
    case strongkv::ClientValueType::kSimpleString:
    case strongkv::ClientValueType::kBulkString:
        std::cout << prefix << value.text << '\n';
        break;
    case strongkv::ClientValueType::kError:
        std::cout << prefix << "(error) " << value.text << '\n';
        break;
    case strongkv::ClientValueType::kInteger:
        std::cout << prefix << "(integer) " << value.integer << '\n';
        break;
    case strongkv::ClientValueType::kNull:
        std::cout << prefix << "(nil)\n";
        break;
    case strongkv::ClientValueType::kArray:
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            std::cout << prefix << i + 1U << ") ";
            print_value(value.array[i], indent + 3U);
        }
        break;
    }
}

void usage(const char* executable) {
    std::cerr
        << "Usage: " << executable
        << " [-h host] [-p port] [--seed host:port] [-a password]"
           " [command ...]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string port = "7401";
    std::string password;
    std::vector<std::string> seeds;
    std::vector<std::string> command;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "-h" || argument == "-p" ||
             argument == "-a" || argument == "--seed") &&
            i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (argument == "-h") {
            host = argv[++i];
        } else if (argument == "-p") {
            port = argv[++i];
        } else if (argument == "-a") {
            password = argv[++i];
        } else if (argument == "--seed") {
            seeds.emplace_back(argv[++i]);
        } else if (argument == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            for (; i < argc; ++i) {
                command.emplace_back(argv[i]);
            }
        }
    }
    if (seeds.empty()) {
        seeds.push_back(host + ":" + port);
    }

    try {
        strongkv::Client client(std::move(seeds));
        client.connect();
        if (!password.empty()) {
            client.auth(password);
        }
        if (!command.empty()) {
            print_value(client.command(command));
            return 0;
        }

        std::string line;
        while (std::cout << "strongkv> " &&
               std::getline(std::cin, line)) {
            auto arguments = split_command(line);
            if (arguments.empty()) {
                continue;
            }
            std::string name = arguments.front();
            for (auto& c : name) {
                c = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(c)));
            }
            if (name == "QUIT" || name == "EXIT") {
                break;
            }
            if (name == "AUTH" && arguments.size() == 2) {
                client.auth(arguments[1]);
                std::cout << "OK\n";
                continue;
            }
            print_value(client.command(arguments));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "(error) " << error.what() << '\n';
        return 1;
    }
}
