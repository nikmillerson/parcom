#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <map>
#include <openssl/ssl.h>
#include <vector>
#include <set>
#include <queue>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

class HttpClient {
private:
    int sock;               // Дескриптор сокета
    SSL_CTX* ssl_ctx;       // Контекст SSL
    SSL* ssl;               // SSL соединение
    bool use_https;         // Флаг использования HTTPS
    std::string connected_host;
    int connected_port;
    std::string resolved_host;
    std::string resolved_ip;

    static constexpr int CONNECT_TIMEOUT_MS = 5000;
    static constexpr int SOCKET_TIMEOUT_SECONDS = 10;

    // Структура для хранения компонентов URL
    struct ParsedUrl {
        std::string protocol;
        std::string host;
        int port;
        std::string path;
        std::string query;
    };

    // Внутренние методы
    ParsedUrl parseUrl(const std::string& url);
    static std::string resolveIpv4(const std::string& host);
    void createSocket(const std::string& host, int port);
    void initSSL();
    void setupSSL(const std::string& host);
    void ensureConnection(const ParsedUrl& parsed);
    void sendData(const std::string& data);
    std::string receiveData();
    void closeConnection();

    // Конструктор для рабочих потоков с уже разрешённым DNS-адресом.
    HttpClient(const std::string& host, const std::string& ip);

public:
    // Конструктор и деструктор
    HttpClient();
    ~HttpClient();

    // Основные методы для выполнения запросов
    std::string get(const std::string& url,
                   const std::map<std::string, std::string>& headers = {});

    std::string post(const std::string& url, const std::string& data,
                    const std::map<std::string, std::string>& headers = {},
                    const std::string& content_type = "application/x-www-form-urlencoded");
    // Функция для извлечения ссылок из HTML
    std::vector<std::string> extractLinks(const std::string& page);
    // Функция для нормализации URL
    std::string normalizeUrl(const std::string& baseUrl, const std::string& link);
    // Функция для извлечения домена из URL
    std::string getDomain(const std::string& url);
    // Основная функция обхода сайта
    void crawlWebsite(const std::string& startUrl, int maxPages = 100,
                      int threadCount = 4);
};

#endif // HTTP_CLIENT_H
