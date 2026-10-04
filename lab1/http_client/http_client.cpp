#include "http_client.h"
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <openssl/err.h>
#include <libxml/uri.h>
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>

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

// Создание и подключение сокета к указанному хосту
void HttpClient::createSocket(const std::string& host, int port) {
    struct addrinfo hints, *result;

    // Обнуляем структуру hints
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // Используем IPv4
    hints.ai_socktype = SOCK_STREAM; // Используем потоковый сокет (TCP)

    // Преобразуем доменное имя в IP-адрес
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        throw std::runtime_error("Ошибка разрешения доменного имени: " + host);
    }

    // Создаем сокет
    sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(result);
        throw std::runtime_error("Ошибка создания сокета");
    }

    // Подключаемся к серверу
    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        close(sock);
        sock = -1;
        throw std::runtime_error("Ошибка подключения к серверу: " + host);
    }

    freeaddrinfo(result);
}

// Инициализация SSL контекста
void HttpClient::initSSL() {
    // Загружаем криптографические алгоритмы и строки ошибок
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

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

// Отправка данных через сокет или SSL
void HttpClient::sendData(const std::string& data) {
    int bytes_sent;
    if (use_https) {
        bytes_sent = SSL_write(ssl, data.c_str(), data.length());
    } else {
        bytes_sent = send(sock, data.c_str(), data.length(), 0);
    }

    if (bytes_sent <= 0) {
        throw std::runtime_error("Ошибка отправки данных");
    }
}

// Получение данных из сокета или SSL
std::string HttpClient::receiveData() {
    std::string response;
    char buffer[4096];
    int bytes_received;

    while (true) {
        if (use_https) {
            bytes_received = SSL_read(ssl, buffer, sizeof(buffer));
        } else {
            bytes_received = recv(sock, buffer, sizeof(buffer), 0);
        }

        if (bytes_received > 0) {
            response.append(buffer, bytes_received);
        } else if (bytes_received == 0) {
            // Соединение закрыто
            break;
        } else {
            throw std::runtime_error("Ошибка получения данных");
        }
    }

    return response;
}

// Конструктор
HttpClient::HttpClient() : sock(-1), ssl_ctx(nullptr), ssl(nullptr), use_https(false) {
    // Инициализируем OpenSSL
    SSL_library_init();
}

// Деструктор
HttpClient::~HttpClient() {
    // Закрываем SSL соединение и освобождаем ресурсы
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }

    // Освобождаем SSL контекст
    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
    }

    // Закрываем сокет
    if (sock >= 0) {
        close(sock);
    }

    // Глобальная очистка OpenSSL здесь не выполняется: при параллельной
    // загрузке другой поток всё ещё может использовать библиотеку.
}

// Выполнение HTTP GET запроса
std::string HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    // Парсим URL
    ParsedUrl parsed = parseUrl(url);
    use_https = (parsed.protocol == "https");

    // Создаем и подключаем сокет
    createSocket(parsed.host, parsed.port);

    // Если используем HTTPS, инициализируем SSL
    if (use_https) {
        initSSL();
        setupSSL(parsed.host);
    }

    // Формируем путь с параметрами, если они есть
    std::string full_path = parsed.path;
    if (!parsed.query.empty()) {
        full_path += "?" + parsed.query;
    }

    // Формируем HTTP-запрос
    std::stringstream request;
    request << "GET " << full_path << " HTTP/1.1\r\n";
    request << "Host: " << parsed.host << "\r\n";
    request << "Connection: close\r\n";
    request << "User-Agent: Simple-CPP-HttpClient/1.0\r\n";

    // Добавляем заголовки
    for (const auto& header : headers) {
        request << header.first << ": " << header.second << "\r\n";
    }

    request << "\r\n";  // Конец заголовков

    // Отправляем запрос
    sendData(request.str());

    // Получаем ответ
    std::string response = receiveData();

    return response;
}

// Выполнение HTTP POST запроса
std::string HttpClient::post(const std::string& url, const std::string& data,
                            const std::map<std::string, std::string>& headers,
                            const std::string& content_type) {
    // Парсим URL
    ParsedUrl parsed = parseUrl(url);
    use_https = (parsed.protocol == "https");

    // Создаем и подключаем сокет
    createSocket(parsed.host, parsed.port);

    // Если используем HTTPS, инициализируем SSL
    if (use_https) {
        initSSL();
        setupSSL(parsed.host);
    }

    // Формируем HTTP-запрос
    std::stringstream request;
    request << "POST " << parsed.path << " HTTP/1.1\r\n";
    request << "Host: " << parsed.host << "\r\n";
    request << "Connection: close\r\n";
    request << "User-Agent: HttpClient/1.0\r\n";
    request << "Content-Type: " << content_type << "\r\n";
    request << "Content-Length: " << data.length() << "\r\n";

    // Добавляем заголовки
    for (const auto& header : headers) {
        request << header.first << ": " << header.second << "\r\n";
    }

    request << "\r\n";  // Конец заголовков
    request << data;    // Тело запроса

    // Отправляем запрос
    sendData(request.str());

    // Получаем ответ
    std::string response = receiveData();

    return response;
}

std::vector<std::string> HttpClient::extractLinks(const std::string &page)
{
    std::vector<std::string> links;
    
    // Парсим HTML
    htmlDocPtr doc = htmlReadDoc((const xmlChar*)page.c_str(), 
                                NULL, NULL, HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (doc == NULL) {
        std::cerr << "Не удалось распарсить HTML" << std::endl;
        return links;
    }
    
    // Создаем контекст XPath
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (context == NULL) {
        std::cerr << "Ошибка создания XPath контекста" << std::endl;
        xmlFreeDoc(doc);
        return links;
    }
    
    // Выполняем XPath запрос для поиска всех ссылок
    xmlXPathObjectPtr result = xmlXPathEvalExpression((const xmlChar*)"//a/@href", context);
    if (result == NULL) {
        std::cerr << "Ошибка выполнения XPath запроса" << std::endl;
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
    std::string domain = getDomain(startUrl);

    std::cout << "Начинаем обход сайта: " << startUrl << std::endl;
    std::cout << "Домен: " << domain << std::endl;

    auto worker = [&]() {
        try {
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
                    // У каждого запроса свой объект: сокет и SSL не разделяются
                    // между потоками.
                    HttpClient requestClient;
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

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    siteMap[currentUrl] = internalLinks;

                    std::cout << "Обработано: " << currentUrl;
                    if (!error.empty()) {
                        std::cout << " (ошибка: " << error << ")";
                    }
                    std::cout << std::endl;

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

                    // И состояние, и уведомление находятся под тем же
                    // мьютексом, поэтому пробуждение не может потеряться.
                    condition.notify_all();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!threadException) {
                threadException = std::current_exception();
            }
            stopped = true;
            condition.notify_all();
        }
    };

    std::vector<std::thread> workers;
    try {
        for (int i = 0; i < threadCount; ++i) {
            workers.emplace_back(worker);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        threadException = std::current_exception();
        stopped = true;
        condition.notify_all();
    }

    for (std::thread& workerThread : workers) {
        workerThread.join();
    }

    if (threadException) {
        std::rethrow_exception(threadException);
    }

    saveToXml(domain, siteMap);
    std::cout << "Обход завершен. Обработано страниц: " << siteMap.size() << std::endl;
    std::cout << "Карта сайта сохранена в sitemap.xml" << std::endl;
}
