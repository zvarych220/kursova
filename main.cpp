// Compile: g++ -std=c++17 -o logger main.cpp

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <optional>
#include <limits>
#include <functional>

// ─── Log Level ───────────────────────────────────────────────────────────────

enum class LogLevel { DEBUG, INFO, WARNING, ERROR };

inline std::string levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
    }
    return "UNKNOWN";
}

inline LogLevel stringToLevel(const std::string& s) {
    if (s == "DEBUG")   return LogLevel::DEBUG;
    if (s == "INFO")    return LogLevel::INFO;
    if (s == "WARNING") return LogLevel::WARNING;
    if (s == "ERROR")   return LogLevel::ERROR;
    throw std::invalid_argument("Unknown log level: " + s);
}

// ─── Output Format ───────────────────────────────────────────────────────────

enum class OutputFormat { TXT, CSV, JSON };

// ─── Colors ──────────────────────────────────────────────────────────────────

namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string RED     = "\033[31m";
    const std::string CYAN    = "\033[36m";

    inline std::string forLevel(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG:   return CYAN;
            case LogLevel::INFO:    return GREEN;
            case LogLevel::WARNING: return YELLOW;
            case LogLevel::ERROR:   return RED;
        }
        return RESET;
    }
}

// ─── LogEntry ────────────────────────────────────────────────────────────────

struct LogEntry {
    LogLevel   level;
    std::string source;
    std::string message;
    std::chrono::system_clock::time_point timestamp;

    std::string formattedTime() const {
        std::time_t t = std::chrono::system_clock::to_time_t(timestamp);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
};

// ─── LogConfig ───────────────────────────────────────────────────────────────

struct LogConfig {
    LogLevel     minLevel      = LogLevel::DEBUG;
    OutputFormat format        = OutputFormat::TXT;
    std::string  filePath      = "app.log";
    size_t       maxFileSizeKB = 1024; // 1 MB default
};

// ─── LogStorage ──────────────────────────────────────────────────────────────

class LogStorage {
public:
    static constexpr size_t MAX_RECENT = 100;

    // 2.1 — add entry to all containers
    void add(LogEntry entry) {
        byLevel[entry.level].push_back(entry);
        bySource[entry.source].push_back(entry);

        if (recent.size() >= MAX_RECENT)
            recent.pop_front();
        recent.push_back(entry);

        entries.push_back(std::move(entry));
    }

    // 2.2 — filter by level (from byLevel map)
    std::vector<LogEntry> filter(LogLevel level) const {
        auto it = byLevel.find(level);
        if (it != byLevel.end()) return it->second;
        return {};
    }

    // 2.2 — filter by source (from bySource map)
    std::vector<LogEntry> filterBySource(const std::string& source) const {
        auto it = bySource.find(source);
        if (it != bySource.end()) return it->second;
        return {};
    }

    // 2.2 — filter by time range using std::copy_if over entries
    std::vector<LogEntry> filterByTimeRange(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end) const
    {
        std::vector<LogEntry> result;
        std::copy_if(entries.begin(), entries.end(), std::back_inserter(result),
            [&](const LogEntry& e) {
                return e.timestamp >= start && e.timestamp <= end;
            });
        return result;
    }

    // 2.3 — sorted copy by timestamp
    std::vector<LogEntry> sortedByTime() const {
        std::vector<LogEntry> copy = entries;
        std::sort(copy.begin(), copy.end(),
            [](const LogEntry& a, const LogEntry& b) {
                return a.timestamp < b.timestamp;
            });
        return copy;
    }

    // 2.3 — sorted copy by source
    std::vector<LogEntry> sortedBySource() const {
        std::vector<LogEntry> copy = entries;
        std::sort(copy.begin(), copy.end(),
            [](const LogEntry& a, const LogEntry& b) {
                return a.source < b.source;
            });
        return copy;
    }

    const std::vector<LogEntry>& getEntries() const { return entries; }
    const std::deque<LogEntry>&  getRecent()  const { return recent; }

private:
    std::vector<LogEntry>                          entries;
    std::deque<LogEntry>                           recent;
    std::map<LogLevel, std::vector<LogEntry>>      byLevel;
    std::map<std::string, std::vector<LogEntry>>   bySource;
};

// ─── LogFormatter ────────────────────────────────────────────────────────────

class LogFormatter {
public:
    // Req 3.1 — TXT: "[time] [LEVEL] [source] message"
    std::string toTXT(const LogEntry& e) const {
        return "[" + e.formattedTime() + "] [" + levelToString(e.level) + "] [" + e.source + "] " + e.message;
    }

    // Req 3.2 — CSV: "time,LEVEL,source,message"
    std::string toCSV(const LogEntry& e) const {
        return e.formattedTime() + "," + levelToString(e.level) + "," + e.source + "," + e.message;
    }

    std::string csvHeader() const {
        return "time,level,source,message";
    }

    // Req 3.3 — JSON: {"time":"...","level":"...","source":"...","message":"..."}
    std::string toJSON(const LogEntry& e) const {
        return "{\"time\":\"" + e.formattedTime() +
               "\",\"level\":\"" + levelToString(e.level) +
               "\",\"source\":\"" + e.source +
               "\",\"message\":\"" + e.message + "\"}";
    }

    // Dispatcher by format
    std::string format(const LogEntry& e, OutputFormat fmt) const {
        switch (fmt) {
            case OutputFormat::TXT:  return toTXT(e);
            case OutputFormat::CSV:  return toCSV(e);
            case OutputFormat::JSON: return toJSON(e);
        }
        return toTXT(e);
    }
};

// ─── LogFileWriter ───────────────────────────────────────────────────────────

class LogFileWriter {
public:
    explicit LogFileWriter(const LogConfig& cfg)
        : config(cfg), currentFileSize(0) {}

    void configure(const LogConfig& cfg) {
        config = cfg;
        currentFileSize = existingFileSize(config.filePath);
    }

    // Req 3.1–3.3, 3.4 — format and append one entry to the log file
    void write(const LogEntry& entry, const LogFormatter& formatter) {
        try {
            rotateIfNeeded();
            std::ofstream ofs(config.filePath, std::ios::app);
            if (!ofs.is_open()) {
                std::cerr << "[LogFileWriter] Cannot open file: " << config.filePath << "\n";
                return;
            }
            std::string line = formatter.format(entry, config.format) + "\n";
            ofs << line;
            currentFileSize += line.size();
        } catch (const std::exception& ex) {
            std::cerr << "[LogFileWriter] write error: " << ex.what() << "\n";
        }
    }

    // Req 3.1–3.3 — write all entries to a new file in the given format
    void saveAll(const std::vector<LogEntry>& entries,
                 const LogFormatter& formatter,
                 OutputFormat fmt) {
        try {
            std::string path = savedFileName(fmt);
            std::ofstream ofs(path);
            if (!ofs.is_open()) {
                std::cerr << "[LogFileWriter] Cannot open file: " << path << "\n";
                return;
            }
            if (fmt == OutputFormat::CSV) {
                ofs << formatter.csvHeader() << "\n";
            } else if (fmt == OutputFormat::JSON) {
                ofs << "[\n";
            }
            for (size_t i = 0; i < entries.size(); ++i) {
                std::string line = formatter.format(entries[i], fmt);
                if (fmt == OutputFormat::JSON) {
                    ofs << "  " << line;
                    if (i + 1 < entries.size()) ofs << ",";
                    ofs << "\n";
                } else {
                    ofs << line << "\n";
                }
            }
            if (fmt == OutputFormat::JSON) {
                ofs << "]\n";
            }
        } catch (const std::exception& ex) {
            std::cerr << "[LogFileWriter] saveAll error: " << ex.what() << "\n";
        }
    }

private:
    LogConfig config;
    size_t    currentFileSize;

    // Req 3.4 — rotate if current file exceeds maxFileSizeKB
    void rotateIfNeeded() {
        if (currentFileSize < config.maxFileSizeKB * 1024) return;
        try {
            std::string rotated = buildRotatedName();
            std::rename(config.filePath.c_str(), rotated.c_str());
            currentFileSize = 0;
        } catch (const std::exception& ex) {
            std::cerr << "[LogFileWriter] rotate error: " << ex.what() << "\n";
        }
    }

    // Build a rotated filename by appending a timestamp before the extension
    std::string buildRotatedName() const {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::string ts = oss.str();

        auto dot = config.filePath.rfind('.');
        if (dot == std::string::npos)
            return config.filePath + "_" + ts;
        return config.filePath.substr(0, dot) + "_" + ts + config.filePath.substr(dot);
    }

    // Build output filename for saveAll (e.g. "app.csv", "app.json")
    std::string savedFileName(OutputFormat fmt) const {
        auto dot = config.filePath.rfind('.');
        std::string base = (dot == std::string::npos) ? config.filePath : config.filePath.substr(0, dot);
        switch (fmt) {
            case OutputFormat::TXT:  return base + ".txt";
            case OutputFormat::CSV:  return base + ".csv";
            case OutputFormat::JSON: return base + ".json";
        }
        return base + ".txt";
    }

    // Get size of an existing file (0 if not found)
    static size_t existingFileSize(const std::string& path) {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        if (!f.is_open()) return 0;
        return static_cast<size_t>(f.tellg());
    }
};

// ─── LogAnalyzer ─────────────────────────────────────────────────────────────

class LogAnalyzer {
public:
    // Req 5.1, 5.2, 5.3, 5.4 — print stats to stdout
    void printStats(const LogStorage& storage) const {
        const auto& entries = storage.getEntries();

        // Req 5.1 — count per level
        std::map<LogLevel, int> levelCount;
        for (const auto& e : entries)
            levelCount[e.level]++;

        std::cout << "=== Log Statistics ===\n";
        std::cout << "Total entries: " << entries.size() << "\n\n";

        std::cout << "By level:\n";
        for (auto level : { LogLevel::DEBUG, LogLevel::INFO, LogLevel::WARNING, LogLevel::ERROR })
            std::cout << "  " << levelToString(level) << ": " << levelCount[level] << "\n";

        // Req 5.2 — count per source
        std::map<std::string, int> sourceCount;
        for (const auto& e : entries)
            sourceCount[e.source]++;

        std::cout << "\nBy source:\n";
        for (const auto& [src, cnt] : sourceCount)
            std::cout << "  " << src << ": " << cnt << "\n";

        // Req 5.3 — find last ERROR using reverse iterator + std::find_if
        auto it = std::find_if(entries.rbegin(), entries.rend(),
            [](const LogEntry& e) { return e.level == LogLevel::ERROR; });

        std::cout << "\nLast ERROR: ";
        if (it != entries.rend())
            std::cout << "[" << it->formattedTime() << "] [" << it->source << "] " << it->message << "\n";
        else
            std::cout << "(none)\n";

        std::cout << "======================\n";
    }
};

// ─── LogLoader ───────────────────────────────────────────────────────────────

// Loads entries from an existing log file into storage on startup.
// Auto-detects format: TXT ([time] [LEVEL] [source] msg) or CSV (time,LEVEL,source,msg)
class LogLoader {
public:
    static void load(const std::string& path, LogStorage& storage) {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return;

        std::string line;
        bool first = true;
        bool isTXT = false;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            if (first) {
                // detect format by first char
                isTXT = (line[0] == '[');
                // skip CSV header row
                if (!isTXT && line.find("time") == 0) continue;
                first = false;
            }
            auto entry = isTXT ? parseTXT(line) : parseCSV(line);
            if (entry) storage.add(std::move(*entry));
        }
    }

private:
    static std::optional<LogEntry> parseTXT(const std::string& line) {
        // [YYYY-MM-DD HH:MM:SS] [LEVEL] [source] message
        if (line.size() < 10 || line[0] != '[') return std::nullopt;

        auto closeTime = line.find(']');
        if (closeTime == std::string::npos) return std::nullopt;
        std::string timeStr = line.substr(1, closeTime - 1);

        auto openLevel = line.find('[', closeTime + 1);
        auto closeLevel = line.find(']', openLevel + 1);
        if (openLevel == std::string::npos || closeLevel == std::string::npos) return std::nullopt;
        std::string levelStr = line.substr(openLevel + 1, closeLevel - openLevel - 1);

        auto openSrc = line.find('[', closeLevel + 1);
        auto closeSrc = line.find(']', openSrc + 1);
        if (openSrc == std::string::npos || closeSrc == std::string::npos) return std::nullopt;
        std::string source = line.substr(openSrc + 1, closeSrc - openSrc - 1);
        std::string message = (closeSrc + 2 < line.size()) ? line.substr(closeSrc + 2) : "";

        return makeEntry(timeStr, levelStr, source, message);
    }

    static std::optional<LogEntry> parseCSV(const std::string& line) {
        // YYYY-MM-DD HH:MM:SS,LEVEL,source,message
        std::vector<std::string> parts;
        std::istringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ','))
            parts.push_back(token);
        if (parts.size() < 4) return std::nullopt;
        // message may contain commas — rejoin remaining parts
        std::string message = parts[3];
        for (size_t i = 4; i < parts.size(); ++i) message += "," + parts[i];
        return makeEntry(parts[0], parts[1], parts[2], message);
    }

    static std::optional<LogEntry> makeEntry(const std::string& timeStr,
                                              const std::string& levelStr,
                                              const std::string& source,
                                              const std::string& message) {
        LogLevel level;
        try { level = stringToLevel(levelStr); } catch (...) { return std::nullopt; }

        std::tm tm{};
        std::istringstream ss(timeStr);
        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ss.fail()) return std::nullopt;
        std::time_t t = std::mktime(&tm);

        LogEntry e;
        e.level     = level;
        e.source    = source;
        e.message   = message;
        e.timestamp = std::chrono::system_clock::from_time_t(t);
        return e;
    }
};

// ─── Auth ────────────────────────────────────────────────────────────────────

enum class Role { ADMIN, SUPERADMIN };

inline std::string roleToString(Role r) {
    return r == Role::SUPERADMIN ? "superadmin" : "admin";
}

inline Role stringToRole(const std::string& s) {
    if (s == "superadmin") return Role::SUPERADMIN;
    return Role::ADMIN;
}

// Simple deterministic hash (FNV-1a 64-bit → hex string, no external deps)
inline std::string hashPassword(const std::string& password) {
    return password; // plain text — no hashing
}

struct User {
    std::string username;
    std::string password;
    Role        role;
};

// Read password (plain input, visible)
inline std::string readPassword() {
    std::string pwd;
    std::getline(std::cin, pwd);
    return pwd;
}
class UserManager {
public:
    explicit UserManager(const std::string& filePath = "users.json")
        : usersFile(filePath) {}

    void load() {
        users.clear();
        std::ifstream ifs(usersFile);
        if (!ifs.is_open()) return;
        std::string line;
        while (std::getline(ifs, line)) {
            auto u = parseLine(line);
            if (u) users.push_back(*u);
        }
    }

    void save() const {
        std::ofstream ofs(usersFile);
        ofs << "[\n";
        for (size_t i = 0; i < users.size(); ++i) {
            ofs << "  {\"username\": \"" << users[i].username
                << "\", \"password\": \"" << users[i].password
                << "\", \"role\": \"" << roleToString(users[i].role) << "\"}";
            if (i + 1 < users.size()) ofs << ",";
            ofs << "\n";
        }
        ofs << "]\n";
    }
    std::optional<User> authenticate(const std::string& username,
                                     const std::string& password) const {
        for (const auto& u : users)
            if (u.username == username && u.password == password)
                return u;
        return std::nullopt;
    }

    bool addUser(const std::string& username, const std::string& password, Role role) {
        for (const auto& u : users)
            if (u.username == username) return false;
        users.push_back({ username, password, role });
        save();
        return true;
    }

    bool removeUser(const std::string& username) {
        auto it = std::remove_if(users.begin(), users.end(),
            [&](const User& u) { return u.username == username; });
        if (it == users.end()) return false;
        users.erase(it, users.end());
        save();
        return true;
    }

    const std::vector<User>& getUsers() const { return users; }

private:
    std::string       usersFile;
    std::vector<User> users;

    static std::optional<User> parseLine(const std::string& line) {
        // expects: {"username": "x", "password": "y", "role": "z"}
        auto extract = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\": \"";
            auto pos = line.find(search);
            if (pos == std::string::npos) return "";
            pos += search.size();
            auto end = line.find('"', pos);
            if (end == std::string::npos) return "";
            return line.substr(pos, end - pos);
        };
        std::string uname = extract("username");
        std::string pwd   = extract("password");
        std::string role  = extract("role");
        if (uname.empty() || pwd.empty() || role.empty()) return std::nullopt;
        return User{ uname, pwd, stringToRole(role) };
    }
};

// ─── Logger (Singleton) ──────────────────────────────────────────────────────

class Logger {
public:
    // Req 7.1 — static access point
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // Req 6.1–6.4 — update config and propagate to writer
    void configure(const LogConfig& cfg) {
        config = cfg;
        writer.configure(cfg);
    }

    // Req 7.1, 6.1, 8.3 — create entry, filter by minLevel, store and write
    void log(LogLevel level, const std::string& source, const std::string& message) {
        try {
            if (level < config.minLevel) return;
            LogEntry entry{ level, source, message, std::chrono::system_clock::now() };
            storage.add(entry);
            writer.write(entry, formatter);
        } catch (const std::exception& ex) {
            std::cerr << "[Logger] log error: " << ex.what() << "\n";
        }
    }

    // Req 5.1–5.4 — delegate to analyzer
    void printStats() {
        analyzer.printStats(storage);
    }

    // Req 3.1–3.3 — delegate to writer
    void saveAll(OutputFormat fmt) {
        writer.saveAll(storage.getEntries(), formatter, fmt);
    }

    // Req 7.1 — read-only access to storage
    const LogStorage& getStorage() const { return storage; }

    // Load existing TXT log file into storage (without re-writing to file)
    void loadFromFile(const std::string& path) {
        LogLoader::load(path, storage);
    }

private:
    Logger() : writer(config) {}
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    LogConfig    config;
    LogStorage   storage;
    LogFormatter formatter;
    LogAnalyzer  analyzer;
    LogFileWriter writer;
};

// ─── CLI ─────────────────────────────────────────────────────────────────────

class CLI {
public:
    CLI(Logger& logger, UserManager& userManager)
        : logger(logger), userManager(userManager) {}

    // Login loop — returns false if user quits
    bool login() {
        while (true) {
            std::system("clear");
            std::cout << "=== STL Logging System — Login ===\n\n";
            std::cout << "Username (or 'q' to quit): ";
            std::string username;
            std::getline(std::cin, username);
            if (username == "q") return false;

            std::cout << "Password: ";
            std::string password = readPassword();

            auto user = userManager.authenticate(username, password);
            if (user) {
                currentUser = user;
                return true;
            }
            std::cout << Color::RED << "Invalid credentials. Try again." << Color::RESET << "\n";
            pause();
        }
    }

    void run() {
        while (true) {
            clearScreen();
            showMenu();
            std::string input;
            std::getline(std::cin, input);
            if (input.empty()) { std::cout << "Invalid option. Try again.\n"; pause(); continue; }
            int choice = 0;
            try { choice = std::stoi(input); } catch (...) {
                std::cout << "Invalid option. Try again.\n"; pause(); continue;
            }
            switch (choice) {
                case 1: handleAddEntry();   break;
                case 2: handleViewLogs();   break;
                case 3: handleSortLogs();   break;
                case 4: handleStatistics(); break;
                case 5: handleSaveLogs();   break;
                case 6:
                    if (isSuperAdmin()) handleConfigure();
                    else { std::cout << "Access denied.\n"; pause(); }
                    break;
                case 7:
                    if (isSuperAdmin()) handleManageUsers();
                    else { std::cout << "Access denied.\n"; pause(); }
                    break;
                case 0: std::cout << "Goodbye.\n"; return;
                default: std::cout << "Invalid option. Try again.\n"; pause(); break;
            }
        }
    }

private:
    Logger&      logger;
    UserManager& userManager;
    std::optional<User> currentUser;

    bool isSuperAdmin() const {
        return currentUser && currentUser->role == Role::SUPERADMIN;
    }

    void clearScreen() const { std::system("clear"); }

    void pause() const {
        std::cout << "\nPress Enter to continue...";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    void showMenu() const {
        std::string roleLabel = currentUser
            ? Color::CYAN + "[" + roleToString(currentUser->role) + ": " + currentUser->username + "]" + Color::RESET
            : "";
        std::cout << "=== STL Logging System === " << roleLabel << "\n"
                  << "1. Add log entry\n"
                  << "2. View logs\n"
                  << "3. Sort logs\n"
                  << "4. Statistics\n"
                  << "5. Save logs to file\n";
        if (isSuperAdmin()) {
            std::cout << "6. Configure logger\n"
                      << "7. Manage users\n";
        }
        std::cout << "0. Exit\n"
                  << "Choice: ";
    }

    LogLevel promptLevel() const {
        while (true) {
            std::cout << "Level (DEBUG/INFO/WARNING/ERROR): ";
            std::string s;
            std::getline(std::cin, s);
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            try { return stringToLevel(s); }
            catch (...) { std::cout << "Invalid level. Try again.\n"; }
        }
    }

    OutputFormat promptFormat() const {
        while (true) {
            std::cout << "Format (TXT/CSV/JSON): ";
            std::string s;
            std::getline(std::cin, s);
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (s == "TXT")  return OutputFormat::TXT;
            if (s == "CSV")  return OutputFormat::CSV;
            if (s == "JSON") return OutputFormat::JSON;
            std::cout << "Invalid format. Try again.\n";
        }
    }

    void printEntries(const std::vector<LogEntry>& entries) const {
        if (entries.empty()) { std::cout << "(no entries)\n"; pause(); return; }

        static constexpr size_t PAGE_SIZE = 20;
        LogFormatter fmt;
        size_t total = entries.size();
        size_t page  = 0;

        while (true) {
            clearScreen();
            size_t from = page * PAGE_SIZE;
            size_t to   = std::min(from + PAGE_SIZE, total);

            std::cout << "Entries " << (from + 1) << "-" << to << " of " << total << ":\n\n";
            for (size_t i = from; i < to; ++i) {
                const auto& e = entries[i];
                std::cout << Color::forLevel(e.level) << fmt.toTXT(e) << Color::RESET << "\n";
            }

            bool hasPrev = (page > 0);
            bool hasNext = (to < total);
            if (!hasPrev && !hasNext) { pause(); return; }

            std::cout << "\n[";
            if (hasPrev) std::cout << "p=prev  ";
            if (hasNext) std::cout << "n=next  ";
            std::cout << "q=back]: ";

            std::string cmd;
            std::getline(std::cin, cmd);
            if (cmd == "n" && hasNext) ++page;
            else if (cmd == "p" && hasPrev) --page;
            else if (cmd == "q") return;
        }
    }

    void handleAddEntry() {
        clearScreen();
        std::cout << "-- Add log entry --\n";
        LogLevel level = promptLevel();

        std::string source;
        while (true) {
            std::cout << "Source: ";
            std::getline(std::cin, source);
            if (!source.empty()) break;
            std::cout << "Source cannot be empty.\n";
        }
        std::string message;
        while (true) {
            std::cout << "Message: ";
            std::getline(std::cin, message);
            if (!message.empty()) break;
            std::cout << "Message cannot be empty.\n";
        }
        logger.log(level, source, message);
        std::cout << "Entry added.\n";
        pause();
    }

    void handleViewLogs() {
        clearScreen();
        std::cout << "-- View logs --\n"
                  << "1. View all\n"
                  << "2. Filter by level\n"
                  << "3. Filter by source\n"
                  << "4. Filter by time range\n"
                  << "Choice: ";
        std::string input;
        std::getline(std::cin, input);
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}

        const LogStorage& storage = logger.getStorage();
        switch (choice) {
            case 1: printEntries(storage.getEntries()); break;
            case 2: { LogLevel lv = promptLevel(); printEntries(storage.filter(lv)); break; }
            case 3: {
                std::cout << "Source: ";
                std::string src; std::getline(std::cin, src);
                printEntries(storage.filterBySource(src));
                break;
            }
            case 4: {
                auto parseTime = [](const std::string& s) {
                    std::tm tm{}; std::istringstream ss(s);
                    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                    if (ss.fail()) throw std::invalid_argument("bad time");
                    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
                };
                std::chrono::system_clock::time_point start, end;
                while (true) {
                    std::cout << "Start (YYYY-MM-DD HH:MM:SS): ";
                    std::string s; std::getline(std::cin, s);
                    try { start = parseTime(s); break; }
                    catch (...) { std::cout << "Invalid format.\n"; }
                }
                while (true) {
                    std::cout << "End   (YYYY-MM-DD HH:MM:SS): ";
                    std::string s; std::getline(std::cin, s);
                    try { end = parseTime(s); break; }
                    catch (...) { std::cout << "Invalid format.\n"; }
                }
                printEntries(storage.filterByTimeRange(start, end));
                break;
            }
            default: std::cout << "Invalid option.\n"; pause(); break;
        }
    }

    void handleSortLogs() {
        clearScreen();
        std::cout << "-- Sort logs --\n"
                  << "1. Sort by timestamp\n"
                  << "2. Sort by source\n"
                  << "Choice: ";
        std::string input; std::getline(std::cin, input);
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}
        const LogStorage& storage = logger.getStorage();
        switch (choice) {
            case 1: printEntries(storage.sortedByTime());   break;
            case 2: printEntries(storage.sortedBySource()); break;
            default: std::cout << "Invalid option.\n"; pause(); break;
        }
    }

    void handleStatistics() {
        clearScreen();
        logger.printStats();
        pause();
    }

    void handleSaveLogs() {
        clearScreen();
        std::cout << "-- Save logs --\n";
        OutputFormat fmt = promptFormat();
        logger.saveAll(fmt);
        std::cout << "Logs saved.\n";
        pause();
    }

    void handleConfigure() {
        clearScreen();
        std::cout << "-- Configure logger --\n";
        LogConfig cfg;
        cfg.minLevel = promptLevel();
        cfg.format   = promptFormat();
        std::cout << "File path: ";
        std::getline(std::cin, cfg.filePath);
        if (cfg.filePath.empty()) cfg.filePath = "app.log";
        while (true) {
            std::cout << "Max file size (KB): ";
            std::string s; std::getline(std::cin, s);
            try {
                int kb = std::stoi(s);
                if (kb > 0) { cfg.maxFileSizeKB = static_cast<size_t>(kb); break; }
            } catch (...) {}
            std::cout << "Invalid value.\n";
        }
        logger.configure(cfg);
        std::cout << "Logger configured.\n";
        pause();
    }

    void handleManageUsers() {
        while (true) {
            clearScreen();
            std::cout << "-- Manage users --\n"
                      << "1. List users\n"
                      << "2. Add user\n"
                      << "3. Remove user\n"
                      << "0. Back\n"
                      << "Choice: ";
            std::string input; std::getline(std::cin, input);
            int choice = 0;
            try { choice = std::stoi(input); } catch (...) {}

            switch (choice) {
                case 0: return;
                case 1: {
                    clearScreen();
                    std::cout << "-- Users --\n";
                    for (const auto& u : userManager.getUsers())
                        std::cout << "  " << Color::CYAN << u.username << Color::RESET
                                  << "  [" << roleToString(u.role) << "]\n";
                    pause();
                    break;
                }
                case 2: {
                    clearScreen();
                    std::cout << "-- Add user --\n";
                    std::string uname;
                    std::cout << "Username: "; std::getline(std::cin, uname);
                    std::cout << "Password: ";
                    std::string pwd = readPassword();
                    std::cout << "Role (admin/superadmin): ";
                    std::string roleStr; std::getline(std::cin, roleStr);
                    Role role = stringToRole(roleStr);
                    if (userManager.addUser(uname, pwd, role))
                        std::cout << Color::GREEN << "User added.\n" << Color::RESET;
                    else
                        std::cout << Color::RED << "Username already exists.\n" << Color::RESET;
                    pause();
                    break;
                }
                case 3: {
                    clearScreen();
                    std::cout << "-- Remove user --\n";
                    std::string uname;
                    std::cout << "Username: "; std::getline(std::cin, uname);
                    if (uname == currentUser->username) {
                        std::cout << Color::RED << "Cannot remove yourself.\n" << Color::RESET;
                    } else if (userManager.removeUser(uname)) {
                        std::cout << Color::GREEN << "User removed.\n" << Color::RESET;
                    } else {
                        std::cout << Color::RED << "User not found.\n" << Color::RESET;
                    }
                    pause();
                    break;
                }
                default: std::cout << "Invalid option.\n"; pause(); break;
            }
        }
    }
};

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Resolve directory of the executable so we find users.json next to it
    std::string exeDir;
    if (argc > 0) {
        std::string exePath(argv[0]);
        auto slash = exePath.rfind('/');
        exeDir = (slash != std::string::npos) ? exePath.substr(0, slash + 1) : "./";
    } else {
        exeDir = "./";
    }

    // Load users
    UserManager userManager(exeDir + "users.json");
    userManager.load();

    // Default logger config: INFO, TXT, app.log, 1024 KB
    LogConfig cfg;
    cfg.minLevel      = LogLevel::INFO;
    cfg.format        = OutputFormat::TXT;
    cfg.filePath      = exeDir + "app.log";
    cfg.maxFileSizeKB = 1024;
    Logger::instance().configure(cfg);
    Logger::instance().loadFromFile(cfg.filePath);

    CLI cli(Logger::instance(), userManager);
    if (!cli.login()) {
        std::cout << "Bye.\n";
        return 0;
    }
    cli.run();
    return 0;
}
