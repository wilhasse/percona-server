#include <mutex>
#include "wiredtiger.h"

class WTConnectionManager {
private:
    static WT_CONNECTION* conn;
    static std::mutex conn_mutex;

    WTConnectionManager() {}  // Private constructor to prevent instantiation

public:
    static WT_CONNECTION* getConnection() {
        std::lock_guard<std::mutex> lock(conn_mutex);
        if (conn == nullptr) {
            int ret = wiredtiger_open(".", NULL, "create", &conn);
            if (ret != 0) {
                // Handle error
                return nullptr;
            }
        }
        return conn;
    }

    static void closeConnection() {
        std::lock_guard<std::mutex> lock(conn_mutex);
        if (conn != nullptr) {
            conn->close(conn, NULL);
            conn = nullptr;
        }
    }
};

WT_CONNECTION* WTConnectionManager::conn = nullptr;
std::mutex WTConnectionManager::conn_mutex;