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
#include <thread>
#include <atomic>
#include <mutex>
#include <random>

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

inline std::ostream& operator<<(std::ostream& os, OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::TXT:  return os << "TXT";
        case OutputFormat::CSV:  return os << "CSV";
        case OutputFormat::JSON: return os << "JSON";
    }
    return os << "UNKNOWN";
}

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

// ─── Flyweight (LogEntryMeta pool) ───────────────────────────────────────────
// Незмінна частина запису (source + level-рядок) — розділяється між усіма записами

struct LogEntryMeta {
    std::string source;
    std::string levelStr;
};

// Пул Flyweight — повертає спільний об'єкт для однакових (source, level) пар
class LogMetaPool {
public:
    static LogMetaPool& instance() {
        static LogMetaPool pool;
        return pool;
    }

    // Повертає вказівник на існуючий або щойно створений об'єкт метаданих
    const LogEntryMeta* get(const std::string& source, LogLevel level) {
        std::string key = source + "|" + levelToString(level);
        auto it = pool_.find(key);
        if (it != pool_.end()) return &it->second;
        pool_[key] = LogEntryMeta{ source, levelToString(level) };
        return &pool_[key];
    }

    size_t poolSize() const { return pool_.size(); }

private:
    std::map<std::string, LogEntryMeta> pool_;
};

// ─── LogStorage ──────────────────────────────────────────────────────────────

class LogStorage {
public:
    static constexpr size_t MAX_RECENT = 100;

    // 2.1 — add entry to all containers
    void add(LogEntry entry) {
        // Flyweight: реєструємо метадані у пулі при кожному додаванні запису
        LogMetaPool::instance().get(entry.source, entry.level);

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

    // Clear all entries from memory
    void clearAll() {
        entries.clear();
        recent.clear();
        byLevel.clear();
        bySource.clear();
    }

    // Remove entries whose timestamp falls within [start, end]
    size_t clearByDateRange(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end)
    {
        auto before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const LogEntry& e) {
                return e.timestamp >= start && e.timestamp <= end;
            }), entries.end());
        size_t removed = before - entries.size();
        // Rebuild index maps and recent from scratch
        byLevel.clear();
        bySource.clear();
        recent.clear();
        for (const auto& e : entries) {
            byLevel[e.level].push_back(e);
            bySource[e.source].push_back(e);
            if (recent.size() >= MAX_RECENT) recent.pop_front();
            recent.push_back(e);
        }
        return removed;
    }

private:
    std::vector<LogEntry>                          entries;
    std::deque<LogEntry>                           recent;
    std::map<LogLevel, std::vector<LogEntry>>      byLevel;
    std::map<std::string, std::vector<LogEntry>>   bySource;
};

// ─── LogFormatter (Factory Method) ──────────────────────────────────────────
// ILogFormatter — абстрактний продукт Factory Method

class ILogFormatter {
public:
    virtual ~ILogFormatter() = default;
    virtual std::string format(const LogEntry& e) const = 0;
    virtual std::string header() const { return ""; }          // опціональний заголовок (CSV)
    virtual std::string prologue() const { return ""; }        // перед списком (JSON "[")
    virtual std::string epilogue() const { return ""; }        // після списку  (JSON "]")
    virtual bool needsIndexSeparator() const { return false; } // JSON потребує ком між елементами
};

// Конкретні продукти

class TXTFormatter : public ILogFormatter {
public:
    // useColor: чи додавати ANSI-кольори до рядка виводу
    explicit TXTFormatter(bool useColor = false) : useColor_(useColor) {}

    std::string format(const LogEntry& e) const override {
        std::string line = "[" + e.formattedTime() + "] [" + levelToString(e.level) + "] [" + e.source + "] " + e.message;
        if (useColor_)
            return Color::forLevel(e.level) + line + Color::RESET;
        return line;
    }

private:
    bool useColor_;
};

class CSVFormatter : public ILogFormatter {
public:
    // delimiter: символ-розділювач колонок (за замовчуванням ',')
    explicit CSVFormatter(char delimiter = ',') : delimiter_(delimiter) {}

    std::string format(const LogEntry& e) const override {
        std::string d(1, delimiter_);
        return e.formattedTime() + d + levelToString(e.level) + d + e.source + d + e.message;
    }
    std::string header() const override {
        std::string d(1, delimiter_);
        return "time" + d + "level" + d + "source" + d + "message";
    }

private:
    char delimiter_;
};

class JSONFormatter : public ILogFormatter {
public:
    // indent: кількість пробілів для відступу елементів масиву
    explicit JSONFormatter(int indent = 2) : indent_(indent) {}

    std::string format(const LogEntry& e) const override {
        return "{\"time\":\"" + e.formattedTime() +
               "\",\"level\":\"" + levelToString(e.level) +
               "\",\"source\":\"" + e.source +
               "\",\"message\":\"" + e.message + "\"}";
    }
    std::string prologue() const override { return "[\n"; }
    std::string epilogue() const override { return "]\n"; }
    bool needsIndexSeparator() const override { return true; }

    // використовується при записі у файл для відступу
    std::string indentStr() const { return std::string(indent_, ' '); }

private:
    int indent_;
};

// FormatterFactory — абстрактний творець (Factory Method)

class FormatterFactory {
public:
    // outputFormat: зберігає прив'язку фабрики до конкретного формату виводу
    explicit FormatterFactory(OutputFormat fmt) : outputFormat_(fmt) {}
    virtual ~FormatterFactory() = default;
    virtual std::unique_ptr<ILogFormatter> createFormatter() const = 0;
    OutputFormat outputFormat() const { return outputFormat_; }

private:
    OutputFormat outputFormat_;
};

class TXTFormatterFactory : public FormatterFactory {
public:
    // useColor: передається у TXTFormatter для увімкнення кольорового виводу
    explicit TXTFormatterFactory(bool useColor = false)
        : FormatterFactory(OutputFormat::TXT), useColor_(useColor) {}
    std::unique_ptr<ILogFormatter> createFormatter() const override {
        return std::make_unique<TXTFormatter>(useColor_);
    }
private:
    bool useColor_;
};

class CSVFormatterFactory : public FormatterFactory {
public:
    // delimiter: передається у CSVFormatter як символ-розділювач
    explicit CSVFormatterFactory(char delimiter = ',')
        : FormatterFactory(OutputFormat::CSV), delimiter_(delimiter) {}
    std::unique_ptr<ILogFormatter> createFormatter() const override {
        return std::make_unique<CSVFormatter>(delimiter_);
    }
private:
    char delimiter_;
};

class JSONFormatterFactory : public FormatterFactory {
public:
    // indent: передається у JSONFormatter як розмір відступу
    explicit JSONFormatterFactory(int indent = 2)
        : FormatterFactory(OutputFormat::JSON), indent_(indent) {}
    std::unique_ptr<ILogFormatter> createFormatter() const override {
        return std::make_unique<JSONFormatter>(indent_);
    }
private:
    int indent_;
};

// Хелпер: отримати фабрику за enum
inline std::unique_ptr<FormatterFactory> makeFormatterFactory(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::CSV:  return std::make_unique<CSVFormatterFactory>();
        case OutputFormat::JSON: return std::make_unique<JSONFormatterFactory>();
        default:                 return std::make_unique<TXTFormatterFactory>();
    }
}
// Зворотна сумісність: LogFormatter-обгортка для старого коду
class LogFormatter {
public:
    std::string format(const LogEntry& e, OutputFormat fmt) const {
        auto f = makeFormatterFactory(fmt)->createFormatter();
        return f->format(e);
    }
    std::string csvHeader() const { return "time,level,source,message"; }
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

// ─── Abstract Factory (OutputFactory) ───────────────────────────────────────
// Створює узгоджену пару: форматер + стратегію запису у файл.
// Кожна конкретна фабрика гарантує, що формат форматера і логіка запису збігаються.

class IOutputFactory {
public:
    virtual ~IOutputFactory() = default;
    virtual std::unique_ptr<ILogFormatter> createFormatter() const = 0;
    virtual OutputFormat                   getFormat()       const = 0;
    // Записує всі entries у файл відповідно до формату фабрики
    virtual void saveAll(const std::vector<LogEntry>& entries,
                         const std::string& basePath) const {
        auto fmt       = getFormat();
        auto formatter = createFormatter();

        // визначаємо розширення
        std::string ext;
        switch (fmt) {
            case OutputFormat::CSV:  ext = ".csv";  break;
            case OutputFormat::JSON: ext = ".json"; break;
            default:                 ext = ".txt";  break;
        }
        auto dot  = basePath.rfind('.');
        std::string path = (dot == std::string::npos ? basePath : basePath.substr(0, dot)) + ext;

        std::ofstream ofs(path);
        if (!ofs.is_open()) { std::cerr << "[OutputFactory] Cannot open: " << path << "\n"; return; }

        if (!formatter->header().empty())   ofs << formatter->header() << "\n";
        if (!formatter->prologue().empty()) ofs << formatter->prologue();

        for (size_t i = 0; i < entries.size(); ++i) {
            std::string line = formatter->format(entries[i]);
            if (formatter->needsIndexSeparator()) {
                ofs << "  " << line;
                if (i + 1 < entries.size()) ofs << ",";
                ofs << "\n";
            } else {
                ofs << line << "\n";
            }
        }
        if (!formatter->epilogue().empty()) ofs << formatter->epilogue();
    }
};

class TXTOutputFactory : public IOutputFactory {
public:
    std::unique_ptr<ILogFormatter> createFormatter() const override { return std::make_unique<TXTFormatter>(); }
    OutputFormat getFormat() const override { return OutputFormat::TXT; }
};

class CSVOutputFactory : public IOutputFactory {
public:
    std::unique_ptr<ILogFormatter> createFormatter() const override { return std::make_unique<CSVFormatter>(); }
    OutputFormat getFormat() const override { return OutputFormat::CSV; }
};

class JSONOutputFactory : public IOutputFactory {
public:
    std::unique_ptr<ILogFormatter> createFormatter() const override { return std::make_unique<JSONFormatter>(); }
    OutputFormat getFormat() const override { return OutputFormat::JSON; }
};

// Хелпер: отримати фабрику виводу за enum
inline std::unique_ptr<IOutputFactory> makeOutputFactory(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::CSV:  return std::make_unique<CSVOutputFactory>();
        case OutputFormat::JSON: return std::make_unique<JSONOutputFactory>();
        default:                 return std::make_unique<TXTOutputFactory>();
    }
}

// ─── Decorator (ILogFormatter decorators) ────────────────────────────────────
// Базовий декоратор — зберігає посилання на обгорнутий форматер

class FormatterDecorator : public ILogFormatter {
public:
    explicit FormatterDecorator(std::unique_ptr<ILogFormatter> inner)
        : inner_(std::move(inner)) {}

    std::string format(const LogEntry& e) const override { return inner_->format(e); }
    std::string header()   const override { return inner_->header(); }
    std::string prologue() const override { return inner_->prologue(); }
    std::string epilogue() const override { return inner_->epilogue(); }
    bool needsIndexSeparator() const override { return inner_->needsIndexSeparator(); }

protected:
    std::unique_ptr<ILogFormatter> inner_;
};

// Конкретний декоратор 1: переводить повідомлення у верхній регістр
class UpperCaseDecorator : public FormatterDecorator {
public:
    using FormatterDecorator::FormatterDecorator;

    std::string format(const LogEntry& e) const override {
        LogEntry upper = e;
        for (auto& c : upper.message)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return inner_->format(upper);
    }
};

// Конкретний декоратор 2: додає префікс із порядковим номером запису
class NumberedDecorator : public FormatterDecorator {
public:
    explicit NumberedDecorator(std::unique_ptr<ILogFormatter> inner)
        : FormatterDecorator(std::move(inner)), counter_(0) {}

    std::string format(const LogEntry& e) const override {
        return "#" + std::to_string(++counter_) + " " + inner_->format(e);
    }

private:
    mutable int counter_;
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

        // Flyweight: показуємо кількість унікальних об'єктів метаданих у пулі
        std::cout << "\nFlyweight pool (unique source+level pairs): "
                  << LogMetaPool::instance().poolSize() << "\n";

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

// ─── Observer ────────────────────────────────────────────────────────────────
// ILogObserver — інтерфейс спостерігача.
// Реалізації отримують сповіщення щоразу, коли до сховища додається новий запис.

class ILogObserver {
public:
    virtual ~ILogObserver() = default;
    virtual void onEntryAdded(const LogEntry& entry) = 0;
};

// Конкретний спостерігач 1: виводить у консоль кожен новий запис
class ConsoleObserver : public ILogObserver {
public:
    void onEntryAdded(const LogEntry& entry) override {
        std::cout << Color::forLevel(entry.level)
                  << "[ConsoleObserver] New entry: ["
                  << levelToString(entry.level) << "] ["
                  << entry.source << "] " << entry.message
                  << Color::RESET << "\n";
    }
};

// Конкретний спостерігач 2: реагує лише на ERROR — виводить попередження
class AlertObserver : public ILogObserver {
public:
    void onEntryAdded(const LogEntry& entry) override {
        if (entry.level == LogLevel::ERROR) {
            std::cout << Color::RED
                      << "[AlertObserver] *** ALERT *** ERROR detected from ["
                      << entry.source << "]: " << entry.message
                      << Color::RESET << "\n";
        }
    }
};

// Конкретний спостерігач 3: рахує кількість записів кожного рівня
class CounterObserver : public ILogObserver {
public:
    void onEntryAdded(const LogEntry& entry) override {
        counts_[entry.level]++;
    }

    void printCounts() const {
        std::cout << "[CounterObserver] Counts: ";
        for (auto lvl : { LogLevel::DEBUG, LogLevel::INFO, LogLevel::WARNING, LogLevel::ERROR })
            std::cout << levelToString(lvl) << "=" << counts_.at(lvl) << " ";
        std::cout << "\n";
    }

private:
    std::map<LogLevel, int> counts_ = {
        { LogLevel::DEBUG, 0 }, { LogLevel::INFO, 0 },
        { LogLevel::WARNING, 0 }, { LogLevel::ERROR, 0 }
    };
};

// LogEventSource — суб'єкт (Subject): керує списком спостерігачів
// і сповіщає їх при кожному новому записі.
class LogEventSource {
public:
    void subscribe(ILogObserver* observer) {
        observers_.push_back(observer);
    }

    void unsubscribe(ILogObserver* observer) {
        observers_.erase(
            std::remove(observers_.begin(), observers_.end(), observer),
            observers_.end());
    }

    void notify(const LogEntry& entry) {
        for (auto* obs : observers_)
            obs->onEntryAdded(entry);
    }

private:
    std::vector<ILogObserver*> observers_;
};

// ─── Visitor ─────────────────────────────────────────────────────────────────
// ILogVisitor — інтерфейс відвідувача.
// Дозволяє додавати нові операції над LogEntry без зміни його структури.

class ILogVisitor {
public:
    virtual ~ILogVisitor() = default;
    virtual void visit(const LogEntry& entry) = 0;
    virtual void finalize() {}  // викликається після обходу всіх записів
};

// Конкретний відвідувач 1: збирає статистику по рівнях та джерелах
class StatsVisitor : public ILogVisitor {
public:
    void visit(const LogEntry& entry) override {
        levelCounts_[entry.level]++;
        sourceCounts_[entry.source]++;
        total_++;
    }

    void finalize() override {
        std::cout << "=== StatsVisitor Report ===\n";
        std::cout << "Total: " << total_ << "\n";
        std::cout << "By level:\n";
        for (auto lvl : { LogLevel::DEBUG, LogLevel::INFO, LogLevel::WARNING, LogLevel::ERROR })
            std::cout << "  " << levelToString(lvl) << ": " << levelCounts_[lvl] << "\n";
        std::cout << "By source:\n";
        for (const auto& [src, cnt] : sourceCounts_)
            std::cout << "  " << src << ": " << cnt << "\n";
        std::cout << "===========================\n";
    }

private:
    std::map<LogLevel, int>      levelCounts_ = {
        { LogLevel::DEBUG, 0 }, { LogLevel::INFO, 0 },
        { LogLevel::WARNING, 0 }, { LogLevel::ERROR, 0 }
    };
    std::map<std::string, int>   sourceCounts_;
    int                          total_ = 0;
};

// Конкретний відвідувач 2: фільтрує записи за рівнем і збирає їх у список
class FilterVisitor : public ILogVisitor {
public:
    explicit FilterVisitor(LogLevel targetLevel) : targetLevel_(targetLevel) {}

    void visit(const LogEntry& entry) override {
        if (entry.level == targetLevel_)
            matched_.push_back(entry);
    }

    const std::vector<LogEntry>& results() const { return matched_; }

private:
    LogLevel              targetLevel_;
    std::vector<LogEntry> matched_;
};

// Конкретний відвідувач 3: серіалізує записи у plain-text рядок (для звіту)
class ReportVisitor : public ILogVisitor {
public:
    void visit(const LogEntry& entry) override {
        report_ += "[" + entry.formattedTime() + "] "
                 + "[" + levelToString(entry.level) + "] "
                 + "[" + entry.source + "] "
                 + entry.message + "\n";
    }

    const std::string& report() const { return report_; }

private:
    std::string report_;
};

// Хелпер: обійти всі записи сховища відвідувачем
inline void acceptVisitor(const LogStorage& storage, ILogVisitor& visitor) {
    for (const auto& entry : storage.getEntries())
        visitor.visit(entry);
    visitor.finalize();
}

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

    // Observer: підписати/відписати спостерігача
    void subscribe(ILogObserver* observer)   { eventSource_.subscribe(observer); }
    void unsubscribe(ILogObserver* observer) { eventSource_.unsubscribe(observer); }

    // Req 7.1, 6.1, 8.3 — create entry, filter by minLevel, store and write
    void log(LogLevel level, const std::string& source, const std::string& message) {
        try {
            if (level < config.minLevel) return;
            LogEntry entry{ level, source, message, std::chrono::system_clock::now() };
            std::lock_guard<std::mutex> lock(mtx);
            storage.add(entry);
            writer.write(entry, formatter);
            // Observer: сповіщаємо всіх підписників про новий запис
            eventSource_.notify(entry);
        } catch (const std::exception& ex) {
            std::cerr << "[Logger] log error: " << ex.what() << "\n";
        }
    }

    // Req 5.1–5.4 — delegate to analyzer
    void printStats() {
        analyzer.printStats(storage);
    }

    // Req 3.1–3.3 — використовує Abstract Factory для збереження у потрібному форматі
    void saveAll(OutputFormat fmt) {
        auto factory = makeOutputFactory(fmt);
        factory->saveAll(storage.getEntries(), config.filePath);
    }

    // Req 7.1 — read-only access to storage
    const LogStorage& getStorage() const { return storage; }

    // Mutable access for LogStorageProxy
    LogStorage& getStorageMutable() { return storage; }

    // Clear all logs from memory and rewrite the log file as empty
    void clearAll() {
        std::lock_guard<std::mutex> lock(mtx);
        storage.clearAll();
        // Truncate the log file
        std::ofstream ofs(config.filePath, std::ios::trunc);
    }

    // Clear logs in [start, end] from memory and rewrite the log file
    size_t clearByDateRange(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end)
    {
        std::lock_guard<std::mutex> lock(mtx);
        size_t removed = storage.clearByDateRange(start, end);
        // Rewrite log file with remaining entries
        std::ofstream ofs(config.filePath, std::ios::trunc);
        if (ofs.is_open()) {
            for (const auto& e : storage.getEntries())
                ofs << formatter.format(e, config.format) << "\n";
        }
        return removed;
    }

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
    LogEventSource eventSource_;
    mutable std::mutex mtx;
};

// ─── Proxy (LogStorageProxy) ─────────────────────────────────────────────────
// Контролює доступ до LogStorage залежно від ролі користувача.
// ADMIN може лише читати записи; SUPERADMIN має повний доступ (читання + очищення).

class ILogStorageAccess {
public:
    virtual ~ILogStorageAccess() = default;
    virtual std::vector<LogEntry> getEntries()                          const = 0;
    virtual std::vector<LogEntry> filter(LogLevel level)                const = 0;
    virtual std::vector<LogEntry> filterBySource(const std::string& s)  const = 0;
    virtual void                  clearAll()                                  = 0;
};

class LogStorageProxy : public ILogStorageAccess {
public:
    LogStorageProxy(LogStorage& storage, Role role)
        : storage_(storage), role_(role) {}

    std::vector<LogEntry> getEntries() const override {
        return storage_.getEntries();
    }

    std::vector<LogEntry> filter(LogLevel level) const override {
        return storage_.filter(level);
    }

    std::vector<LogEntry> filterBySource(const std::string& s) const override {
        return storage_.filterBySource(s);
    }

    // Тільки SUPERADMIN може очищати сховище
    void clearAll() override {
        if (role_ != Role::SUPERADMIN) {
            std::cout << Color::RED
                      << "[Proxy] Access denied: only superadmin can clear logs.\n"
                      << Color::RESET;
            return;
        }
        storage_.clearAll();
        std::cout << Color::GREEN << "[Proxy] Logs cleared by superadmin.\n" << Color::RESET;
    }

private:
    LogStorage& storage_;
    Role        role_;
};

// ─── Facade (LoggingFacade) ───────────────────────────────────────────────────
// Спрощений інтерфейс для найпоширеніших операцій із системою логування.
// Приховує взаємодію між Logger, LogStorage, LogAnalyzer та IOutputFactory.

class LoggingFacade {
public:
    explicit LoggingFacade(Logger& logger) : logger_(logger) {}

    // Додати запис одним викликом
    void addEntry(LogLevel level, const std::string& source, const std::string& message) {
        logger_.log(level, source, message);
    }

    // Зберегти логи одразу у всіх трьох форматах
    void saveAllFormats(const std::string& /*basePath*/) {
        for (auto fmt : { OutputFormat::TXT, OutputFormat::CSV, OutputFormat::JSON })
            logger_.saveAll(fmt);
    }

    // Показати статистику
    void showStats() {
        logger_.printStats();
    }

    // Отримати останні N записів
    std::vector<LogEntry> getRecent(size_t n) const {
        const auto& entries = logger_.getStorage().getEntries();
        if (entries.size() <= n) return entries;
        return std::vector<LogEntry>(entries.end() - static_cast<std::ptrdiff_t>(n), entries.end());
    }

private:
    Logger& logger_;
};

// ─── Logger (Singleton) ──────────────────────────────────────────────────────

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
                // Facade та Proxy створюються після логіну, коли відома роль
                facade_ = std::make_unique<LoggingFacade>(logger);
                proxy_  = std::make_unique<LogStorageProxy>(
                    logger.getStorageMutable(), currentUser->role);
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
                case 8: handleClearLogs();  break;
                case 9: handleVisitorAnalysis(); break;
                case 0:
                    std::cout << "Goodbye.\n";
                    return;
                default: std::cout << "Invalid option. Try again.\n"; pause(); break;
            }
        }
    }

private:
    Logger&      logger;
    UserManager& userManager;
    std::optional<User> currentUser;
    std::unique_ptr<LoggingFacade>    facade_;
    std::unique_ptr<LogStorageProxy>  proxy_;
    // Observer: спостерігачі активні протягом усієї сесії користувача
    ConsoleObserver consoleObs_;
    AlertObserver   alertObs_;

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
                  << "5. Save logs to file\n"
                  << "8. Clear logs\n"
                  << "9. Visitor: analyze logs\n";
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

        // Decorator: запитуємо режим відображення
        std::cout << "Display mode:\n"
                  << "  1. Normal\n"
                  << "  2. Numbered lines\n"
                  << "  3. UPPERCASE messages\n"
                  << "  4. Numbered + UPPERCASE\n"
                  << "Choice [1]: ";
        std::string modeInput;
        std::getline(std::cin, modeInput);
        int mode = 1;
        try { mode = std::stoi(modeInput); } catch (...) {}

        // Будуємо стек декораторів залежно від вибору
        std::unique_ptr<ILogFormatter> formatter;
        switch (mode) {
            case 2:
                formatter = std::make_unique<NumberedDecorator>(
                    std::make_unique<TXTFormatter>(true));
                break;
            case 3:
                formatter = std::make_unique<UpperCaseDecorator>(
                    std::make_unique<TXTFormatter>(true));
                break;
            case 4:
                formatter = std::make_unique<NumberedDecorator>(
                    std::make_unique<UpperCaseDecorator>(
                        std::make_unique<TXTFormatter>(true)));
                break;
            default:
                formatter = std::make_unique<TXTFormatter>(true);
                break;
        }

        static constexpr size_t PAGE_SIZE = 20;
        size_t total = entries.size();
        size_t page  = 0;

        while (true) {
            clearScreen();
            size_t from = page * PAGE_SIZE;
            size_t to   = std::min(from + PAGE_SIZE, total);

            std::cout << "Entries " << (from + 1) << "-" << to << " of " << total << ":\n\n";
            for (size_t i = from; i < to; ++i) {
                std::cout << formatter->format(entries[i]) << "\n";
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
        // Facade: один виклик замість прямого звернення до logger
        // Observer: підписуємо спостерігачів лише на цей один запис
        logger.subscribe(&consoleObs_);
        logger.subscribe(&alertObs_);
        facade_->addEntry(level, source, message);
        logger.unsubscribe(&consoleObs_);
        logger.unsubscribe(&alertObs_);
        std::cout << "Entry added.\n";
        pause(); // пауза дозволяє побачити вивід спостерігачів перед очищенням екрану
    }

    void handleViewLogs() {
        clearScreen();
        std::cout << "-- View logs --\n"
                  << "1. View all\n"
                  << "2. Filter by level\n"
                  << "3. Filter by source\n"
                  << "4. Filter by time range\n"
                  << "5. Live monitor (Observer)\n"
                  << "Choice: ";
        std::string input;
        std::getline(std::cin, input);
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}

        // Proxy: доступ до сховища через контролер прав
        switch (choice) {
            case 1: printEntries(proxy_->getEntries()); break;
            case 2: { LogLevel lv = promptLevel(); printEntries(proxy_->filter(lv)); break; }
            case 3: {
                std::cout << "Source: ";
                std::string src; std::getline(std::cin, src);
                printEntries(proxy_->filterBySource(src));
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
                printEntries(logger.getStorage().filterByTimeRange(start, end));
                break;
            }
            case 5: handleLiveMonitor(); break;
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
        // Proxy: читання через контролер прав
        switch (choice) {
            case 1: printEntries(logger.getStorage().sortedByTime());   break;
            case 2: printEntries(logger.getStorage().sortedBySource()); break;
            default: std::cout << "Invalid option.\n"; pause(); break;
        }
    }

    void handleStatistics() {
        clearScreen();
        // Facade: один виклик замість logger.printStats()
        facade_->showStats();
        pause();
    }

    void handleSaveLogs() {
        clearScreen();
        std::cout << "-- Save logs --\n";
        OutputFormat fmt = promptFormat();
        // Facade: зберігає у вибраному форматі
        facade_->addEntry(LogLevel::INFO, "system", "Logs exported"); // аудит через facade
        logger.saveAll(fmt);
        std::cout << "Logs saved.\n";
        pause();
    }

    void handleClearLogs() {
        clearScreen();
        std::cout << "-- Clear logs --\n"
                  << "1. Clear all logs\n"
                  << "2. Clear by date range\n"
                  << "0. Back\n"
                  << "Choice: ";
        std::string input; std::getline(std::cin, input);
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}

        auto parseTime = [](const std::string& s) {
            std::tm tm{}; std::istringstream ss(s);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            if (ss.fail()) throw std::invalid_argument("bad time");
            return std::chrono::system_clock::from_time_t(std::mktime(&tm));
        };

        switch (choice) {
            case 1: {
                std::cout << "Are you sure? (yes/no): ";
                std::string confirm; std::getline(std::cin, confirm);
                if (confirm == "yes") {
                    // Proxy: clearAll перевіряє роль — admin отримає відмову
                    proxy_->clearAll();
                } else {
                    std::cout << "Cancelled.\n";
                }
                pause();
                break;
            }
            case 2: {
                std::chrono::system_clock::time_point start, end;
                while (true) {
                    std::cout << "From (YYYY-MM-DD HH:MM:SS): ";
                    std::string s; std::getline(std::cin, s);
                    try { start = parseTime(s); break; }
                    catch (...) { std::cout << "Invalid format.\n"; }
                }
                while (true) {
                    std::cout << "To   (YYYY-MM-DD HH:MM:SS): ";
                    std::string s; std::getline(std::cin, s);
                    try { end = parseTime(s); break; }
                    catch (...) { std::cout << "Invalid format.\n"; }
                }
                size_t removed = logger.clearByDateRange(start, end);
                std::cout << Color::GREEN << "Removed " << removed << " entries.\n" << Color::RESET;
                pause();
                break;
            }
            case 0: break;
            default: std::cout << "Invalid option.\n"; pause(); break;
        }
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
    void handleLiveMonitor() {
        // Observer: LiveObserver накопичує нові записи у потокобезпечній черзі
        struct LiveObserver : public ILogObserver {
            std::mutex              mtx;
            std::vector<LogEntry>   newEntries;
            void onEntryAdded(const LogEntry& entry) override {
                std::lock_guard<std::mutex> lock(mtx);
                newEntries.push_back(entry);
            }
        };

        LiveObserver liveObs;
        logger.subscribe(&liveObs);

        // Показуємо останні 15 записів як початковий стан
        static constexpr size_t VISIBLE = 15;
        std::atomic<bool> stop{false};

        // Окремий потік читає Enter для виходу
        std::thread inputThread([&stop]() {
            std::string s;
            std::getline(std::cin, s);
            stop.store(true);
        });

        TXTFormatter fmt(true);

        while (!stop.load()) {
            clearScreen();
            std::cout << "=== Live Monitor (Observer) === [Enter to exit]\n\n";

            // Беремо останні VISIBLE записів зі сховища
            const auto& all = logger.getStorage().getEntries();
            size_t from = all.size() > VISIBLE ? all.size() - VISIBLE : 0;
            for (size_t i = from; i < all.size(); ++i)
                std::cout << fmt.format(all[i]) << "\n";

            // Показуємо лічильник нових записів з моменту відкриття монітора
            {
                std::lock_guard<std::mutex> lock(liveObs.mtx);
                if (!liveObs.newEntries.empty())
                    std::cout << "\n" << Color::CYAN
                              << "[Observer] +" << liveObs.newEntries.size()
                              << " new since monitor opened"
                              << Color::RESET << "\n";
            }

            std::cout << "\n[auto-refresh every 1s]\n";

            // Чекаємо 1 секунду з можливістю раннього виходу
            for (int i = 0; i < 10 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        inputThread.join();
        logger.unsubscribe(&liveObs);
    }

    void handleVisitorAnalysis() {
        clearScreen();
        std::cout << "-- Visitor: Analyze logs --\n"
                  << "1. Statistics (StatsVisitor)\n"
                  << "2. Filter by level (FilterVisitor)\n"
                  << "3. Generate report (ReportVisitor)\n"
                  << "Choice: ";
        std::string input; std::getline(std::cin, input);
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) {}

        switch (choice) {
            case 1: {
                StatsVisitor sv;
                acceptVisitor(logger.getStorage(), sv);
                break;
            }
            case 2: {
                LogLevel lv = promptLevel();
                FilterVisitor fv(lv);
                acceptVisitor(logger.getStorage(), fv);
                std::cout << "Matched " << fv.results().size()
                          << " entries with level " << levelToString(lv) << ":\n";
                TXTFormatter txt;
                for (const auto& e : fv.results())
                    std::cout << "  " << txt.format(e) << "\n";
                break;
            }
            case 3: {
                ReportVisitor rv;
                acceptVisitor(logger.getStorage(), rv);
                std::cout << rv.report();
                break;
            }
            default: std::cout << "Invalid option.\n"; break;
        }
        pause();
    }
};

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

    // ─── Демонстрація патерну Factory Method ─────────────────────────────────
    // Показує що кожна конкретна фабрика створює свій форматер
    // і форматує один і той самий запис по-різному.
    {
        std::cout << "\n╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║     DEMO: Factory Method (FormatterFactory)  ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        // Тестовий запис журналу
        LogEntry sample;
        sample.level     = LogLevel::WARNING;
        sample.source    = "auth";
        sample.message   = "Failed login attempt";
        sample.timestamp = std::chrono::system_clock::now();

        // Три конкретні творці — кожен створює свій продукт
        std::vector<std::unique_ptr<FormatterFactory>> factories;
        factories.push_back(std::make_unique<TXTFormatterFactory>(false));
        factories.push_back(std::make_unique<CSVFormatterFactory>(','));
        factories.push_back(std::make_unique<JSONFormatterFactory>(2));

        for (const auto& factory : factories) {
            auto formatter = factory->createFormatter();
            std::cout << "[Factory: " << factory->outputFormat() << "]\n";
            if (!formatter->header().empty())
                std::cout << "  header : " << formatter->header() << "\n";
            std::cout << "  format : " << formatter->format(sample) << "\n\n";
        }
    }

    // ─── Демонстрація патерну Abstract Factory ────────────────────────────────
    // Показує що кожна фабрика створює узгоджену пару продуктів
    // (форматер + стратегія запису) і зберігає файл у своєму форматі.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║   DEMO: Abstract Factory (IOutputFactory)   ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        // Тестові записи для збереження
        std::vector<LogEntry> samples;
        for (auto lvl : { LogLevel::INFO, LogLevel::WARNING, LogLevel::ERROR }) {
            LogEntry e;
            e.level     = lvl;
            e.source    = "demo";
            e.message   = "Abstract Factory test entry";
            e.timestamp = std::chrono::system_clock::now();
            samples.push_back(e);
        }

        // Три конкретні фабрики — кожна зберігає файл свого сімейства
        std::vector<std::unique_ptr<IOutputFactory>> outFactories;
        outFactories.push_back(std::make_unique<TXTOutputFactory>());
        outFactories.push_back(std::make_unique<CSVOutputFactory>());
        outFactories.push_back(std::make_unique<JSONOutputFactory>());

        for (const auto& factory : outFactories) {
            factory->saveAll(samples, exeDir + "demo_output");
            std::cout << "[Factory: " << factory->getFormat()
                      << "] -> файл збережено\n";
        }
        std::cout << "\n";
    }

    // ─── Демонстрація патерну Decorator ──────────────────────────────────────
    // Декоратори обгортають існуючі форматери, додаючи нову поведінку
    // без зміни оригінальних класів.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║        DEMO: Decorator (FormatterDecorator) ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        LogEntry sample;
        sample.level     = LogLevel::ERROR;
        sample.source    = "network";
        sample.message   = "connection timeout";
        sample.timestamp = std::chrono::system_clock::now();

        // Базовий TXT форматер
        auto base = std::make_unique<TXTFormatter>(false);
        std::cout << "[Base TXT]       : " << base->format(sample) << "\n";

        // UpperCaseDecorator поверх TXT
        auto upper = std::make_unique<UpperCaseDecorator>(std::make_unique<TXTFormatter>(false));
        std::cout << "[UpperCase+TXT]  : " << upper->format(sample) << "\n";

        // NumberedDecorator поверх CSV
        auto numbered = std::make_unique<NumberedDecorator>(std::make_unique<CSVFormatter>());
        std::cout << "[Numbered+CSV]   : " << numbered->format(sample) << "\n";
        std::cout << "[Numbered+CSV]   : " << numbered->format(sample) << "\n";

        // Стек декораторів: Numbered → UpperCase → TXT
        auto stacked = std::make_unique<NumberedDecorator>(
            std::make_unique<UpperCaseDecorator>(
                std::make_unique<TXTFormatter>(false)));
        std::cout << "[Numbered+Upper] : " << stacked->format(sample) << "\n\n";
    }

    // ─── Демонстрація патерну Flyweight ──────────────────────────────────────
    // LogMetaPool кешує незмінні метадані (source + level) і повертає
    // спільний об'єкт замість створення нового для кожного запису.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║        DEMO: Flyweight (LogMetaPool)        ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        auto& pool = LogMetaPool::instance();

        // Симулюємо 6 записів з 3 унікальними (source, level) парами
        struct RawEntry { std::string source; LogLevel level; std::string message; };
        std::vector<RawEntry> raw = {
            { "auth",    LogLevel::ERROR,   "login failed"       },
            { "auth",    LogLevel::ERROR,   "token expired"      },
            { "network", LogLevel::WARNING, "high latency"       },
            { "network", LogLevel::WARNING, "packet loss"        },
            { "db",      LogLevel::INFO,    "query executed"     },
            { "db",      LogLevel::INFO,    "connection opened"  },
        };

        std::cout << "Entries: " << raw.size()
                  << "  |  Unique meta objects in pool: ";

        std::vector<const LogEntryMeta*> metas;
        for (const auto& r : raw) {
            const LogEntryMeta* meta = pool.get(r.source, r.level);
            metas.push_back(meta);
        }

        std::cout << pool.poolSize() << "\n\n";

        for (size_t i = 0; i < raw.size(); ++i) {
            std::cout << "  [" << metas[i]->levelStr << "] ["
                      << metas[i]->source << "] " << raw[i].message
                      << "  (meta@" << static_cast<const void*>(metas[i]) << ")\n";
        }

        // Перевіряємо що однакові пари повертають той самий об'єкт
        std::cout << "\n  auth/ERROR ptr same? "
                  << (pool.get("auth", LogLevel::ERROR) == pool.get("auth", LogLevel::ERROR)
                      ? "yes (shared)" : "no") << "\n\n";
    }

    // ─── Демонстрація патерну Proxy ──────────────────────────────────────────
    // LogStorageProxy контролює доступ до LogStorage залежно від ролі.
    // admin може лише читати; superadmin має повний доступ.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║        DEMO: Proxy (LogStorageProxy)        ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        // Додаємо тестові записи у сховище
        LogStorage demoStorage;
        LogEntry e1{ LogLevel::INFO, "proxy-demo", "test entry 1", std::chrono::system_clock::now() };
        LogEntry e2{ LogLevel::ERROR, "proxy-demo", "test entry 2", std::chrono::system_clock::now() };
        demoStorage.add(e1);
        demoStorage.add(e2);

        // Proxy для admin — читання дозволено, очищення заблоковано
        LogStorageProxy adminProxy(demoStorage, Role::ADMIN);
        std::cout << "[admin] getEntries count: " << adminProxy.getEntries().size() << "\n";
        std::cout << "[admin] trying clearAll -> ";
        adminProxy.clearAll();

        // Proxy для superadmin — повний доступ
        LogStorageProxy superProxy(demoStorage, Role::SUPERADMIN);
        std::cout << "[superadmin] getEntries count: " << superProxy.getEntries().size() << "\n";
        std::cout << "[superadmin] trying clearAll -> ";
        superProxy.clearAll();
        std::cout << "[superadmin] entries after clear: " << superProxy.getEntries().size() << "\n\n";
    }

    // ─── Демонстрація патерну Facade ─────────────────────────────────────────
    // LoggingFacade спрощує типові операції: додати запис, зберегти у всіх
    // форматах, показати статистику — один виклик замість кількох.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║       DEMO: Facade (LoggingFacade)          ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        LoggingFacade facade(Logger::instance());

        // Один виклик — додати запис
        facade.addEntry(LogLevel::INFO,    "facade-demo", "System started via facade");
        facade.addEntry(LogLevel::WARNING, "facade-demo", "Low disk space via facade");
        facade.addEntry(LogLevel::ERROR,   "facade-demo", "Critical error via facade");

        // Отримати останні 3 записи
        auto recent = facade.getRecent(3);
        std::cout << "Last " << recent.size() << " entries via facade:\n";
        TXTFormatter txt;
        for (const auto& e : recent)
            std::cout << "  " << txt.format(e) << "\n";

        // Зберегти у всіх форматах одним викликом
        facade.saveAllFormats(exeDir + "app");
        std::cout << "\nSaved to TXT/CSV/JSON via facade.\n\n";
    }

    // ─── Демонстрація патерну Observer ───────────────────────────────────────
    // Спостерігачі підписуються на Logger і отримують сповіщення при кожному
    // новому записі. AlertObserver реагує лише на ERROR.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║        DEMO: Observer (LogEventSource)      ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        ConsoleObserver consoleObs;
        AlertObserver   alertObs;
        CounterObserver counterObs;

        Logger::instance().subscribe(&consoleObs);
        Logger::instance().subscribe(&alertObs);
        Logger::instance().subscribe(&counterObs);

        Logger::instance().log(LogLevel::INFO,    "observer-demo", "Service started");
        Logger::instance().log(LogLevel::WARNING, "observer-demo", "Memory usage high");
        Logger::instance().log(LogLevel::ERROR,   "observer-demo", "Database unreachable");
        Logger::instance().log(LogLevel::INFO,    "observer-demo", "Retry succeeded");

        counterObs.printCounts();

        // Відписуємо спостерігачів після демо
        Logger::instance().unsubscribe(&consoleObs);
        Logger::instance().unsubscribe(&alertObs);
        Logger::instance().unsubscribe(&counterObs);
        std::cout << "\n";
    }

    // ─── Демонстрація патерну Visitor ────────────────────────────────────────
    // Відвідувачі обходять записи сховища і виконують різні операції
    // (статистика, фільтрація, звіт) без зміни LogEntry.
    {
        std::cout << "╔══════════════════════════════════════════════╗\n";
        std::cout <<   "║        DEMO: Visitor (ILogVisitor)          ║\n";
        std::cout <<   "╚══════════════════════════════════════════════╝\n";

        // StatsVisitor — підраховує записи по рівнях та джерелах
        StatsVisitor sv;
        acceptVisitor(Logger::instance().getStorage(), sv);

        // FilterVisitor — збирає лише ERROR записи
        FilterVisitor fv(LogLevel::ERROR);
        acceptVisitor(Logger::instance().getStorage(), fv);
        std::cout << "FilterVisitor (ERROR): found " << fv.results().size() << " entries\n";

        // ReportVisitor — будує текстовий звіт
        ReportVisitor rv;
        acceptVisitor(Logger::instance().getStorage(), rv);
        std::cout << "ReportVisitor: report length = " << rv.report().size() << " chars\n\n";
    }

    CLI cli(Logger::instance(), userManager);
    if (!cli.login()) {
        std::cout << "Bye.\n";
        return 0;
    }

    // Background random log generator — fires every 5 seconds
    std::atomic<bool> stopGen{false};
    std::thread logGen([&stopGen]() {
        static const std::vector<std::string> sources = { "auth", "db", "network", "cache", "scheduler" };
        static const std::vector<std::pair<LogLevel, std::string>> messages = {
            { LogLevel::INFO,    "Heartbeat OK" },
            { LogLevel::INFO,    "Cache refreshed" },
            { LogLevel::INFO,    "Scheduled task completed" },
            { LogLevel::WARNING, "High memory usage detected" },
            { LogLevel::WARNING, "Slow query detected" },
            { LogLevel::ERROR,   "Connection pool exhausted" },
            { LogLevel::DEBUG,   "Token validated" },
            { LogLevel::DEBUG,   "Config reloaded" },
        };
        std::mt19937 rng(std::random_device{}());
        while (!stopGen.load()) {
            for (int i = 0; i < 50 && !stopGen.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stopGen.load()) break;
            std::uniform_int_distribution<size_t> srcDist(0, sources.size() - 1);
            std::uniform_int_distribution<size_t> msgDist(0, messages.size() - 1);
            const auto& [lvl, msg] = messages[msgDist(rng)];
            Logger::instance().log(lvl, sources[srcDist(rng)], msg);
        }
    });

    cli.run();
    stopGen.store(true);
    logGen.join();
    return 0;
}
