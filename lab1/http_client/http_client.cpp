#include "http_client.h"
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <openssl/err.h>
#include <libxml/uri.h>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace {

std::mutex consoleMutex;

void printLine(std::ostream& stream, const std::string& text)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    stream << text << std::endl;
}

std::string lowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

// Парсинг URL на компоненты
HttpClient::ParsedUrl HttpClient::parseUrl(const std::string& url) {
    ParsedUrl parsed;

    // Определяем протокол
    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos) {
        parsed.protocol = url.substr(0, protocol_end);
        size_t host_start = protocol_end + 3;

        // Находим конец хоста (после хоста может быть порт, путь или параметры)
        size_t host_end = url.find('/', host_start);
        size_t port_pos = url.find(':', host_start);

        if (host_end == std::string::npos) {
            host_end = url.length();
        }

        // Извлекаем хост и порт если есть
        if (port_pos != std::string::npos && port_pos < host_end) {
            parsed.host = url.substr(host_start, port_pos - host_start);
            size_t port_end = (host_end < url.length()) ? host_end : url.length();
            std::string port_str = url.substr(port_pos + 1, port_end - port_pos - 1);
            parsed.port = std::stoi(port_str);
        } else {
            parsed.host = url.substr(host_start, host_end - host_start);
            parsed.port = (parsed.protocol == "https") ? 443 : 80;
        }

        // Извлекаем путь и параметры
        if (host_end < url.length()) {
            size_t query_pos = url.find('?', host_end);
            if (query_pos != std::string::npos) {
                parsed.path = url.substr(host_end, query_pos - host_end);
                parsed.query = url.substr(query_pos + 1);
            } else {
                parsed.path = url.substr(host_end);
            }
        } else {
            parsed.path = "/";
        }
    } else {
        // Если протокол не указан, используем HTTP по умолчанию
        parsed.protocol = "http";
        size_t host_end = url.find('/');

        if (host_end == std::string::npos) {
            parsed.host = url;
            parsed.path = "/";
        } else {
            parsed.host = url.substr(0, host_end);
            parsed.path = url.substr(host_end);
        }

        parsed.port = 80;
    }

    return parsed;
}

std::string HttpClient::resolveIpv4(const std::string& host)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const int status = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (status != 0) {
        throw std::runtime_error("Ошибка разрешения доменного имени: " + host);
    }

    char address[INET_ADDRSTRLEN]{};
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    const char* converted = inet_ntop(AF_INET, &ipv4->sin_addr,
                                      address, sizeof(address));
    freeaddrinfo(result);
    if (converted == nullptr) {
        throw std::runtime_error("Не удалось преобразовать IP-адрес: " + host);
    }
    return address;
}

// Создание сокета с ограничением времени подключения и сетевых операций.
void HttpClient::createSocket(const std::string& host, int port)
{
    std::string ip;
    if (!resolved_ip.empty()) {
        if (host != resolved_host) {
            throw std::runtime_error("URL вышел за пределы разрешённого домена");
        }
        ip = resolved_ip;
    } else {
        // Обычные get/post сохраняют прежнее поведение. В crawlWebsite DNS
        // разрешается заранее в главном потоке, поэтому работники сюда не входят.
        ip = resolveIpv4(host);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Некорректный IP-адрес: " + ip);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Ошибка создания сокета");
    }

    try {
        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("Не удалось настроить сокет");
        }

        int result = connect(sock, reinterpret_cast<sockaddr*>(&address),
                             sizeof(address));
        if (result < 0 && errno == EINPROGRESS) {
            pollfd descriptor{sock, POLLOUT, 0};
            result = poll(&descriptor, 1, CONNECT_TIMEOUT_MS);
            if (result == 0) {
                throw std::runtime_error("Таймаут подключения к серверу: " + host);
            }
            if (result < 0) {
                throw std::runtime_error("Ошибка ожидания подключения: " + host);
            }

            int socketError = 0;
            socklen_t errorSize = sizeof(socketError);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                           &socketError, &errorSize) < 0 || socketError != 0) {
                throw std::runtime_error("Ошибка подключения к серверу: " + host);
            }
        } else if (result < 0) {
            throw std::runtime_error("Ошибка подключения к серверу: " + host);
        }

        if (fcntl(sock, F_SETFL, flags) < 0) {
            throw std::runtime_error("Не удалось восстановить режим сокета");
        }

        timeval timeout{};
        timeout.tv_sec = SOCKET_TIMEOUT_SECONDS;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) < 0 ||
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout)) < 0) {
            throw std::runtime_error("Не удалось установить таймаут сокета");
        }
    } catch (...) {
        close(sock);
        sock = -1;
        throw;
    }
}

// Инициализация SSL контекста
void HttpClient::initSSL() {
    if (ssl_ctx != nullptr) {
        return;
    }

    // Создаем SSL контекст с методом TLS
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        throw std::runtime_error("Ошибка создания SSL контекста");
    }
}

// Установка SSL соединения
void HttpClient::setupSSL(const std::string& host) {
    // Создаем SSL структуру
    ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        throw std::runtime_error("Ошибка создания SSL структуры");
    }

    // Привязываем SSL к сокету
    if (SSL_set_fd(ssl, sock) == 0) {
        SSL_free(ssl);
        ssl = nullptr;
        throw std::runtime_error("Ошибка привязки SSL к сокету");
    }

    // Устанавливаем имя хоста для проверки сертификата
    if (SSL_set_tlsext_host_name(ssl, host.c_str()) == 0) {
        SSL_free(ssl);
        ssl = nullptr;
        throw std::runtime_error("Ошибка установки имени хоста");
    }

    // Устанавливаем SSL соединение
    if (SSL_connect(ssl) <= 0) {
        SSL_free(ssl);
        ssl = nullptr;
        throw std::runtime_error("Ошибка SSL handshake");
    }
}

void HttpClient::ensureConnection(const ParsedUrl& parsed)
{
    const bool https = parsed.protocol == "https";
    if (sock >= 0 && connected_host == parsed.host &&
        connected_port == parsed.port && use_https == https) {
        return;
    }

    closeConnection();
    use_https = https;
    createSocket(parsed.host, parsed.port);
    try {
        if (use_https) {
            setupSSL(parsed.host);
        }
        connected_host = parsed.host;
        connected_port = parsed.port;
    } catch (...) {
        closeConnection();
        throw;
    }
}

// Отправка данных через сокет или SSL
void HttpClient::sendData(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t remaining = data.size() - sent;
        const int portion = static_cast<int>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int result = use_https
            ? SSL_write(ssl, data.data() + sent, portion)
            : static_cast<int>(send(sock, data.data() + sent, portion, MSG_NOSIGNAL));
        if (result <= 0) {
            throw std::runtime_error("Ошибка или таймаут отправки данных");
        }
        sent += static_cast<std::size_t>(result);
    }
}

// Получение данных из сокета или SSL
std::string HttpClient::receiveData() {
    auto receiveBytes = [&](char* data, std::size_t size) -> int {
        while (true) {
            const int requested = static_cast<int>(std::min<std::size_t>(
                size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
            if (use_https) {
                const int result = SSL_read(ssl, data, requested);
                if (result > 0) {
                    return result;
                }
                const int sslError = SSL_get_error(ssl, result);
                if (sslError == SSL_ERROR_ZERO_RETURN) {
                    return 0;
                }
                if (sslError == SSL_ERROR_SYSCALL && errno == EINTR) {
                    continue;
                }
                if (sslError == SSL_ERROR_SYSCALL &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    throw std::runtime_error("Таймаут получения данных");
                }
                throw std::runtime_error("Ошибка получения HTTPS-данных");
            }

            const int result = static_cast<int>(recv(sock, data, requested, 0));
            if (result >= 0) {
                return result;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw std::runtime_error("Таймаут получения данных");
            }
            throw std::runtime_error("Ошибка получения данных");
        }
    };

    std::string response;
    char temporary[4096];
    std::size_t headerEnd = std::string::npos;
    while ((headerEnd = response.find("\r\n\r\n")) == std::string::npos) {
        const int received = receiveBytes(temporary, sizeof(temporary));
        if (received == 0) {
            throw std::runtime_error("Соединение закрыто до получения HTTP-заголовков");
        }
        response.append(temporary, static_cast<std::size_t>(received));
        if (response.size() > 1024 * 1024) {
            throw std::runtime_error("Слишком большой HTTP-заголовок");
        }
    }

    const std::size_t bodyStart = headerEnd + 4;
    const std::string header = response.substr(0, bodyStart);
    std::string buffered = response.substr(bodyStart);
    std::optional<std::size_t> contentLength;
    std::string transferEncoding;
    std::string connectionHeader;

    const std::size_t statusEnd = header.find("\r\n");
    const std::string statusLine = header.substr(0, statusEnd);
    int statusCode = 0;
    {
        std::istringstream status(statusLine);
        std::string version;
        status >> version >> statusCode;
    }

    std::size_t lineStart = statusEnd == std::string::npos
        ? header.size() : statusEnd + 2;
    while (lineStart < headerEnd) {
        const std::size_t lineEnd = header.find("\r\n", lineStart);
        const std::string line = header.substr(lineStart, lineEnd - lineStart);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string name = lowerAscii(trim(line.substr(0, colon)));
            const std::string value = lowerAscii(trim(line.substr(colon + 1)));
            if (name == "content-length") {
                std::size_t used = 0;
                const unsigned long long length = std::stoull(value, &used);
                if (used != value.size() ||
                    length > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("Некорректный Content-Length");
                }
                contentLength = static_cast<std::size_t>(length);
            } else if (name == "transfer-encoding") {
                transferEncoding = value;
            } else if (name == "connection") {
                connectionHeader = value;
            }
        }
        lineStart = lineEnd + 2;
    }

    const bool http10 = statusLine.rfind("HTTP/1.0", 0) == 0;
    bool reusable = connectionHeader.find("close") == std::string::npos &&
        (!http10 || connectionHeader.find("keep-alive") != std::string::npos);
    const bool noBody = (statusCode >= 100 && statusCode < 200) ||
        statusCode == 204 || statusCode == 304;
    std::string body;

    auto readMore = [&]() {
        const int received = receiveBytes(temporary, sizeof(temporary));
        if (received == 0) {
            return false;
        }
        buffered.append(temporary, static_cast<std::size_t>(received));
        return true;
    };
    auto readLine = [&]() {
        std::size_t end = buffered.find("\r\n");
        while (end == std::string::npos) {
            if (!readMore()) {
                throw std::runtime_error("Соединение закрыто внутри chunked-ответа");
            }
            end = buffered.find("\r\n");
        }
        std::string line = buffered.substr(0, end);
        buffered.erase(0, end + 2);
        return line;
    };
    auto requireBytes = [&](std::size_t count) {
        while (buffered.size() < count) {
            if (!readMore()) {
                throw std::runtime_error("HTTP-ответ завершился раньше ожидаемого");
            }
        }
    };

    if (noBody) {
        buffered.clear();
    } else if (transferEncoding.find("chunked") != std::string::npos) {
        while (true) {
            std::string sizeText = readLine();
            const std::size_t extension = sizeText.find(';');
            if (extension != std::string::npos) {
                sizeText.erase(extension);
            }
            sizeText = trim(sizeText);
            std::size_t used = 0;
            const unsigned long long parsed = std::stoull(sizeText, &used, 16);
            if (used != sizeText.size() ||
                parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("Некорректный размер HTTP-чанка");
            }
            const std::size_t chunkSize = static_cast<std::size_t>(parsed);
            if (chunkSize == 0) {
                while (!readLine().empty()) {
                    // Пропускаем необязательные trailer-заголовки.
                }
                break;
            }
            if (chunkSize > std::numeric_limits<std::size_t>::max() - 2) {
                throw std::runtime_error("Слишком большой HTTP-чанк");
            }
            requireBytes(chunkSize + 2);
            body.append(buffered.data(), chunkSize);
            if (buffered.compare(chunkSize, 2, "\r\n") != 0) {
                throw std::runtime_error("Некорректное завершение HTTP-чанка");
            }
            buffered.erase(0, chunkSize + 2);
        }
    } else if (contentLength) {
        requireBytes(*contentLength);
        body.assign(buffered.data(), *contentLength);
    } else {
        reusable = false;
        body = std::move(buffered);
        while (true) {
            const int received = receiveBytes(temporary, sizeof(temporary));
            if (received == 0) {
                break;
            }
            body.append(temporary, static_cast<std::size_t>(received));
        }
    }

    if (!reusable) {
        closeConnection();
    }
    return header + body;
}

// Конструктор
HttpClient::HttpClient()
    : sock(-1), ssl_ctx(nullptr), ssl(nullptr), use_https(false),
      connected_port(0)
{
    static std::once_flag initialization;
    std::call_once(initialization, []() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        std::signal(SIGPIPE, SIG_IGN);
    });
    initSSL();
}

HttpClient::HttpClient(const std::string& host, const std::string& ip)
    : HttpClient()
{
    resolved_host = host;
    resolved_ip = ip;
}

// Деструктор
HttpClient::~HttpClient() {
    closeConnection();

    // Освобождаем SSL контекст
    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
        ssl_ctx = nullptr;
    }

    // Глобальная очистка OpenSSL здесь не выполняется: при параллельной
    // загрузке другой поток всё ещё может использовать библиотеку.
}

void HttpClient::closeConnection()
{
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = nullptr;
    }
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
    connected_host.clear();
    connected_port = 0;
    use_https = false;
}

// Выполнение HTTP GET запроса
std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    // Парсим URL
    ParsedUrl parsed = parseUrl(url);

    // Формируем путь с параметрами, если они есть
    std::string full_path = parsed.path;
    if (!parsed.query.empty()) {
        full_path += "?" + parsed.query;
    }

    // Формируем HTTP-запрос
    std::stringstream request;
    request << "GET " << full_path << " HTTP/1.1\r\n";
    request << "Host: " << getDomain(url) << "\r\n";
    request << "Connection: keep-alive\r\n";
    request << "User-Agent: Simple-CPP-HttpClient/1.0\r\n";

    // Добавляем заголовки
    for (const auto& header : headers) {
        request << header.first << ": " << header.second << "\r\n";
    }

    request << "\r\n";  // Конец заголовков

    // Если сервер успел закрыть сохранённое keep-alive соединение, GET можно
    // безопасно повторить один раз через новый сокет.
    const bool reused = sock >= 0 && connected_host == parsed.host &&
        connected_port == parsed.port && use_https == (parsed.protocol == "https");
    for (int attempt = 0; attempt < (reused ? 2 : 1); ++attempt) {
        try {
            ensureConnection(parsed);
            sendData(request.str());
            return receiveData();
        } catch (...) {
            closeConnection();
            if (attempt + 1 == (reused ? 2 : 1)) {
                throw;
            }
        }
    }
    throw std::runtime_error("Не удалось выполнить GET-запрос");
}

// Выполнение HTTP POST запроса
std::string HttpClient::post(const std::string& url, const std::string& data,
                            const std::map<std::string, std::string>& headers,
                            const std::string& content_type) {
    // Парсим URL
    ParsedUrl parsed = parseUrl(url);
    ensureConnection(parsed);

    // Формируем HTTP-запрос
    std::stringstream request;
    request << "POST " << parsed.path << " HTTP/1.1\r\n";
    request << "Host: " << getDomain(url) << "\r\n";
    request << "Connection: keep-alive\r\n";
    request << "User-Agent: HttpClient/1.0\r\n";
    request << "Content-Type: " << content_type << "\r\n";
    request << "Content-Length: " << data.length() << "\r\n";

    // Добавляем заголовки
    for (const auto& header : headers) {
        request << header.first << ": " << header.second << "\r\n";
    }

    request << "\r\n";  // Конец заголовков
    request << data;    // Тело запроса

    try {
        sendData(request.str());
        return receiveData();
    } catch (...) {
        closeConnection();
        throw;
    }
}

std::vector<std::string> HttpClient::extractLinks(const std::string &page)
{
    std::vector<std::string> links;

    // Парсим HTML
    htmlDocPtr doc = htmlReadDoc((const xmlChar*)page.c_str(),
                                NULL, NULL, HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (doc == NULL) {
        printLine(std::cerr, "Не удалось распарсить HTML");
        return links;
    }

    // Создаем контекст XPath
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printLine(std::cerr, "Ошибка создания XPath контекста");
        xmlFreeDoc(doc);
        return links;
    }

    // Выполняем XPath запрос для поиска всех ссылок
    xmlXPathObjectPtr result = xmlXPathEvalExpression((const xmlChar*)"//a/@href", context);
    if (result == NULL) {
        printLine(std::cerr, "Ошибка выполнения XPath запроса");
        xmlXPathFreeContext(context);
        xmlFreeDoc(doc);
        return links;
    }

    // Обрабатываем результаты
    xmlNodeSetPtr nodeset = result->nodesetval;
    if (nodeset != NULL) {
        for (int i = 0; i < nodeset->nodeNr; i++) {
            xmlChar* value = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
            if (value != NULL) {
                std::string link((char*)value);
                links.push_back(link);
                xmlFree(value);
            }
        }
    }

    // Освобождаем ресурсы
    xmlXPathFreeObject(result);
    xmlXPathFreeContext(context);
    xmlFreeDoc(doc);

    return links;
}

std::string HttpClient::normalizeUrl(const std::string &baseUrl, const std::string &link)
{
    if (link.empty() || link.find("javascript:") == 0 ||
        link.find("mailto:") == 0 || link.find("tel:") == 0) {
        return "";
    }

    // libxml2 корректно объединяет абсолютные и относительные URL.
    xmlChar* result = xmlBuildURI(
        reinterpret_cast<const xmlChar*>(link.c_str()),
        reinterpret_cast<const xmlChar*>(baseUrl.c_str()));

    if (result == nullptr) {
        return "";
    }

    std::string normalized(reinterpret_cast<const char*>(result));
    xmlFree(result);

    // Фрагмент (#section) не меняет загружаемую страницу.
    size_t fragment = normalized.find('#');
    if (fragment != std::string::npos) {
        normalized.erase(fragment);
    }

    if (normalized.find("http://") != 0 && normalized.find("https://") != 0) {
        return "";
    }
    return normalized;
}
std::string HttpClient::getDomain(const std::string &url)
{
    size_t protocolEnd = url.find("://");
    size_t start = (protocolEnd == std::string::npos) ? 0 : protocolEnd + 3;
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) {
        return url.substr(start);
    }
    return url.substr(start, end - start);
}

namespace {

std::string escapeXml(const std::string& text)
{
    std::string result;
    for (char symbol : text) {
        switch (symbol) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '\"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += symbol;
        }
    }
    return result;
}

void saveToXml(const std::string& domain,
               const std::map<std::string, std::vector<std::string>>& siteMap)
{
    std::ofstream file("sitemap.xml");
    if (!file) {
        throw std::runtime_error("Не удалось создать sitemap.xml");
    }

    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    file << "<sitemap domain=\"" << escapeXml(domain) << "\">\n";
    for (const auto& [page, links] : siteMap) {
        file << "  <page url=\"" << escapeXml(page) << "\">\n";
        for (const std::string& link : links) {
            file << "    <link>" << escapeXml(link) << "</link>\n";
        }
        file << "  </page>\n";
    }
    file << "</sitemap>\n";
}

} // namespace

void HttpClient::crawlWebsite(const std::string &startUrl, int maxPages,
                              int threadCount)
{
    if (maxPages <= 0) {
        std::cout << "Количество страниц должно быть больше нуля" << std::endl;
        return;
    }
    if (threadCount <= 0) {
        threadCount = 1;
    }
    threadCount = std::min(threadCount, maxPages);

    // Глобальное состояние libxml2 и DNS подготавливаются до запуска потоков.
    xmlInitParser();
    const ParsedUrl startParsed = parseUrl(startUrl);
    const std::string domain = getDomain(startUrl);
    const std::string serverIp = resolveIpv4(startParsed.host);

    // visited содержит уже поставленные в очередь адреса. Это не позволяет
    // двум потокам одновременно добавить одну и ту же страницу.
    std::set<std::string> visited;
    std::queue<std::string> toVisit;
    std::map<std::string, std::vector<std::string>> siteMap;
    std::mutex mutex;
    std::condition_variable condition;
    std::exception_ptr threadException;
    bool stopped = false;

    // Число задач в очереди плюс число задач, которые сейчас выполняются.
    size_t pendingTasks = 1;
    toVisit.push(startUrl);
    visited.insert(startUrl);

    printLine(std::cout, "Начинаем обход сайта: " + startUrl);
    printLine(std::cout, "Домен: " + domain);

    auto worker = [&]() {
        try {
            // Один клиент на рабочий поток. Он повторно использует SSL_CTX и
            // keep-alive соединение, а DNS-адрес уже получен главным потоком.
            HttpClient requestClient(startParsed.host, serverIp);
            while (true) {
                std::string currentUrl;

                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [&]() {
                        return stopped || !toVisit.empty() || pendingTasks == 0;
                    });

                    if (stopped || pendingTasks == 0) {
                        return;
                    }

                    currentUrl = toVisit.front();
                    toVisit.pop();
                }

                std::vector<std::string> internalLinks;
                std::string error;

                try {
                    std::string page = requestClient.get(currentUrl);

                    // Отделяем тело ответа от HTTP-заголовков.
                    size_t bodyStart = page.find("\r\n\r\n");
                    if (bodyStart != std::string::npos) {
                        page.erase(0, bodyStart + 4);
                    }

                    std::set<std::string> uniqueLinks;
                    for (const std::string& link : requestClient.extractLinks(page)) {
                        std::string normalizedLink =
                            requestClient.normalizeUrl(currentUrl, link);
                        if (!normalizedLink.empty() &&
                            requestClient.getDomain(normalizedLink) == domain) {
                            uniqueLinks.insert(normalizedLink);
                        }
                    }
                    internalLinks.assign(uniqueLinks.begin(), uniqueLinks.end());
                } catch (const std::exception& exception) {
                    // Ошибка одной страницы не останавливает обход остальных.
                    error = exception.what();
                }

                std::ostringstream message;
                message << "Обработано: " << currentUrl;
                if (!error.empty()) {
                    message << " (ошибка: " << error << ")";
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    siteMap[currentUrl] = internalLinks;

                    for (const std::string& link : internalLinks) {
                        if (visited.size() >= static_cast<size_t>(maxPages)) {
                            break;
                        }
                        if (visited.insert(link).second) {
                            toVisit.push(link);
                            ++pendingTasks;
                        }
                    }

                    --pendingTasks;
                }

                // Предикат изменён под mutex. Уведомляем после освобождения,
                // чтобы разбуженные потоки сразу могли захватить mutex.
                condition.notify_all();
                printLine(std::cout, message.str());
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!threadException) {
                    threadException = std::current_exception();
                }
                stopped = true;
            }
            condition.notify_all();
        }
    };

    std::vector<std::thread> workers;
    try {
        for (int i = 0; i < threadCount; ++i) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            threadException = std::current_exception();
            stopped = true;
        }
        condition.notify_all();
    }

    for (std::thread& workerThread : workers) {
        workerThread.join();
    }

    if (threadException) {
        std::rethrow_exception(threadException);
    }

    saveToXml(domain, siteMap);
    printLine(std::cout, "Обход завершен. Обработано страниц: " +
                         std::to_string(siteMap.size()));
    printLine(std::cout, "Карта сайта сохранена в sitemap.xml");
}
