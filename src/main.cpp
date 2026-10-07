#include "search.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::filesystem::path root;
    std::string start = "index.html";
    fixture_search::Limits limits;
    std::vector<std::string> queries;
    std::size_t benchmark = 0;
    bool json = false;
};

void usage() {
    std::cout <<
        "Usage: fixture-search --root DIR [options]\n"
        "  --start PATH          Seed HTML path within root (default index.html)\n"
        "  --query TEXT          AND query; repeat for multiple queries\n"
        "  --json                Emit one JSON object\n"
        "  --max-pages N         Indexed page limit (default 64)\n"
        "  --max-depth N         Maximum link distance from seed (default 8)\n"
        "  --max-page-bytes N    Byte limit for one page (default 262144)\n"
        "  --max-total-bytes N   Total indexed input bytes (default 4194304)\n"
        "  --max-links N         Total hrefs considered (default 4096)\n"
        "  --benchmark N         Repeat all queries N times (1..10000)\n"
        "  --help                Show this help\n";
}

std::size_t number(const std::string& value, const std::string& option, bool zero = false) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        (!zero && result == 0)) {
        throw std::invalid_argument(option + " needs " + (zero ? "a nonnegative" : "a positive") + " integer");
    }
    return result;
}

void check_output() {
    if (!std::cout.flush()) throw std::runtime_error("cannot write stdout");
}

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") { result.json = true; continue; }
        if (arg != "--root" && arg != "--start" && arg != "--query" &&
            arg != "--max-pages" && arg != "--max-depth" && arg != "--max-page-bytes" &&
            arg != "--max-total-bytes" && arg != "--max-links" && arg != "--benchmark") {
            throw std::invalid_argument("unknown option: " + arg);
        }
        if (++i == argc) throw std::invalid_argument("missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--root") result.root = std::filesystem::u8path(value);
        else if (arg == "--start") result.start = value;
        else if (arg == "--query") result.queries.push_back(value);
        else if (arg == "--max-pages") result.limits.pages = number(value, arg);
        else if (arg == "--max-depth") result.limits.depth = number(value, arg, true);
        else if (arg == "--max-page-bytes") result.limits.page_bytes = number(value, arg);
        else if (arg == "--max-total-bytes") result.limits.total_bytes = number(value, arg);
        else if (arg == "--max-links") result.limits.links = number(value, arg, true);
        else if (arg == "--benchmark") {
            result.benchmark = number(value, arg);
            if (result.benchmark > 10000) throw std::invalid_argument("benchmark limit is 10000 repetitions");
        }
    }
    if (result.root.empty()) throw std::invalid_argument("--root is required");
    if (result.benchmark && result.queries.empty()) {
        throw std::invalid_argument("--benchmark requires at least one --query");
    }
    return result;
}

std::string json_string(const std::string& value) {
    const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

struct Timing {
    double build_ms = 0;
    double query_ms = 0;
    std::uint64_t checksum = 0;
};

void print_json(const Options& opts, const fixture_search::CrawlResult& result,
                const Timing& timing) {
    const auto& docs = result.index.documents();
    std::cout << "{\"stats\":{\"pages\":" << docs.size()
              << ",\"tokens\":" << result.index.tokens()
              << ",\"terms\":" << result.index.terms()
              << ",\"bytes_indexed\":" << result.bytes_indexed
              << ",\"links_seen\":" << result.links_seen
              << ",\"links_omitted\":" << result.links_omitted
              << ",\"duplicate_links\":" << result.duplicate_links << "},\"documents\":[";
    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"path\":" << json_string(docs[i].path)
                  << ",\"title\":" << json_string(docs[i].title)
                  << ",\"tokens\":" << docs[i].tokens << '}';
    }
    std::cout << "],\"skipped\":[";
    for (std::size_t i = 0; i < result.skipped.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"target\":" << json_string(result.skipped[i].target)
                  << ",\"reason\":" << json_string(result.skipped[i].reason) << '}';
    }
    std::cout << "],\"queries\":[";
    for (std::size_t i = 0; i < opts.queries.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"query\":" << json_string(opts.queries[i]) << ",\"hits\":[";
        const auto hits = result.index.search(opts.queries[i]);
        for (std::size_t h = 0; h < hits.size(); ++h) {
            if (h) std::cout << ',';
            const auto& doc = docs[hits[h].document];
            std::cout << "{\"path\":" << json_string(doc.path)
                      << ",\"title\":" << json_string(doc.title)
                      << ",\"score\":" << hits[h].score << '}';
        }
        std::cout << "]}";
    }
    std::cout << ']';
    if (opts.benchmark) {
        std::cout << ",\"benchmark\":{\"repetitions\":" << opts.benchmark
                  << ",\"queries_per_repetition\":" << opts.queries.size()
                  << ",\"build_ms\":" << timing.build_ms
                  << ",\"query_ms\":" << timing.query_ms
                  << ",\"checksum\":" << timing.checksum << '}';
    }
    std::cout << "}\n";
}

void print_text(const Options& opts, const fixture_search::CrawlResult& result,
                const Timing& timing) {
    const auto& docs = result.index.documents();
    std::cout << "Indexed " << docs.size() << " pages, " << result.index.tokens()
              << " tokens, " << result.index.terms() << " terms ("
              << result.bytes_indexed << " input bytes).\n"
              << "Links: " << result.links_seen << " seen, " << result.duplicate_links
              << " duplicates, " << result.links_omitted << " omitted by limit.\n";
    for (const auto& skip : result.skipped) {
        std::cout << "Skipped " << json_string(skip.target) << ": " << skip.reason << '\n';
    }
    for (const auto& query : opts.queries) {
        const auto hits = result.index.search(query);
        std::cout << "\nQuery " << json_string(query) << ": " << hits.size() << " matches\n";
        for (const auto& hit : hits) {
            const auto& doc = docs[hit.document];
            std::cout << "  " << hit.score << "  " << doc.path << "  | " << doc.title << '\n';
        }
    }
    if (opts.benchmark) {
        std::cout << "\nBenchmark: " << opts.benchmark << " repetitions x " << opts.queries.size()
                  << " queries; build=" << timing.build_ms << " ms, queries=" << timing.query_ms
                  << " ms, checksum=" << timing.checksum << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            usage();
            check_output();
            return 0;
        }
        const auto opts = options(argc, argv);
        using Clock = std::chrono::steady_clock;
        const auto build_start = Clock::now();
        const auto result = fixture_search::crawl(opts.root, opts.start, opts.limits);
        Timing timing;
        timing.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();
        if (opts.benchmark) {
            const auto query_start = Clock::now();
            for (std::size_t i = 0; i < opts.benchmark; ++i) {
                for (const auto& query : opts.queries) {
                    for (const auto& hit : result.index.search(query)) {
                        timing.checksum += static_cast<std::uint64_t>(hit.document + 1) * hit.score;
                    }
                }
            }
            timing.query_ms = std::chrono::duration<double, std::milli>(Clock::now() - query_start).count();
        }
        std::cout << std::fixed << std::setprecision(3);
        if (opts.json) print_json(opts, result, timing);
        else print_text(opts, result, timing);
        check_output();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fixture-search: " << error.what() << '\n';
        return 2;
    }
}
