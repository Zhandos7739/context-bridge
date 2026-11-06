// Copyright (c) 2026 ProtocolTeam (Zhandos). All rights reserved.
// Viewing allowed. No copying, modification, commercial use, or redistribution
// without written permission from ProtocolTeam (Zhandos).

#include "bridge.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

constexpr const char* kClause = "details and original text are in the file";
constexpr const char* kProtocol =
    "Context Bridge is on. A line like \"file 123994: user asked about a laptop or tech; "
    "details and original text are in the file\" replaces older turns. The original is in that "
    "numbered file. Read it with tool read_context_file or the token [[ctx:read:123994]]. "
    "Search with search_context or [[ctx:search:query]]. Do not invent file contents.";

const char* kStops[] = {
    "the", "and", "for", "that", "this", "with", "you", "your", "from", "have", "are", "was",
    "were", "but", "not", "can", "could", "would", "about", "into", "what", "when", "where",
    "which", "who", "how", "why", "its", "also", "just", "than", "then", "them", "they",
    "their", "there", "here", "will", "please", "need", "want", "like", "some", "any", "more",
    "user", "assistant", "system", "tool", "asked", "dialog", "file", "files", "details",
    "original", "text", "said", "tell", "help",
    "и", "в", "во", "не", "что", "он", "на", "я", "с", "со", "как", "то", "все", "она", "так",
    "его", "но", "да", "ты", "же", "вы", "за", "бы", "по", "только", "ее", "мне", "было",
    "вот", "от", "меня", "еще", "ещё", "нет", "из", "ему", "когда", "даже", "если", "уже",
    "или", "быть", "был", "него", "до", "вас", "вам", "там", "потом", "себя", "они", "тут",
    "где", "есть", "надо", "для", "мы", "тебя", "их", "чем", "этот", "этого", "мой", "тем",
    "чтобы", "сейчас", "были", "всех", "можно", "при", "про", "это", "очень", "будет",
    "хочу", "нужно", "пожалуйста", "запись", "записи", "номер", "фрагмент"};

bool is_stop(const std::string& word) {
    for (const char* stop : kStops) {
        if (word == stop) return true;
    }
    return false;
}

std::string utf8_lower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c < 128) {
            out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
        } else if (c == 0xD0 && i + 1 < text.size()) {
            auto d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x90 && d <= 0xAF) {
                out.push_back(static_cast<char>(0xD0));
                out.push_back(static_cast<char>(d + 0x20));
            } else if (d == 0x81) {
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(0x91));
            } else {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(d));
            }
            i += 2;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            out.push_back(static_cast<char>(c));
            out.push_back(text[i + 1]);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            out.push_back(static_cast<char>(c));
            out.push_back(text[i + 1]);
            out.push_back(text[i + 2]);
            i += 3;
        } else {
            out.push_back(static_cast<char>(c));
            ++i;
        }
    }
    return out;
}

bool word_byte(unsigned char c) { return std::isalnum(c) || c >= 128; }

std::vector<std::string> tokenize(const std::string& text) {
    std::string lower = utf8_lower(text);
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3 && !is_stop(cur)) {
            bool digits = true;
            for (unsigned char c : cur) {
                if (c < '0' || c > '9') digits = false;
            }
            if (!digits) words.push_back(cur);
        }
        cur.clear();
    };
    for (unsigned char c : lower) {
        if (word_byte(c)) cur.push_back(static_cast<char>(c));
        else flush();
    }
    flush();
    return words;
}

std::vector<std::string> top_keywords(const std::string& text, int k) {
    std::map<std::string, int> counts;
    for (const auto& word : tokenize(text)) ++counts[word];
    std::vector<std::pair<std::string, int>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::vector<std::string> out;
    for (int i = 0; i < k && i < static_cast<int>(ranked.size()); ++i) out.push_back(ranked[i].first);
    return out;
}

std::string article(const std::string& word) {
    if (!word.empty() && word[0] >= 'a' && word[0] <= 'z') {
        const char* a = std::string("aeiou").find(word[0]) != std::string::npos ? "an " : "a ";
        return std::string(a) + word;
    }
    return word;
}

std::string join_topics(const std::vector<std::string>& words) {
    if (words.empty()) return "the earlier dialog";
    if (words.size() == 1) return article(words[0]);
    return article(words[0]) + " or " + words[1];
}

void write_u(std::ostream& out, const std::string& value) { out << value << '\n'; }

std::string read_line(std::istream& in) {
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

void write_block(std::ostream& out, const std::string& value) {
    out << value.size() << '\n';
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    out << '\n';
}

std::string read_block(std::istream& in) {
    std::string len_line = read_line(in);
    if (len_line.empty()) throw std::runtime_error("truncated record");
    size_t len = static_cast<size_t>(std::stoul(len_line));
    std::string data(len, '\0');
    in.read(data.data(), static_cast<std::streamsize>(len));
    if (in.gcount() != static_cast<std::streamsize>(len)) throw std::runtime_error("truncated record");
    if (in.get() != '\n' && !in.eof()) {
        // The byte after the block is a newline we wrote. If the block itself
        // ended without consuming it, put nothing back: get() already ate it.
    }
    return data;
}

void write_message(std::ostream& out, const Message& message) {
    write_u(out, message.role);
    write_u(out, message.kind);
    write_u(out, message.file_id);
    write_u(out, message.pinned ? "1" : "0");
    write_u(out, message.tool_call_id);
    write_u(out, std::to_string(message.tool_calls.size()));
    for (const auto& call : message.tool_calls) {
        write_u(out, call.id);
        write_u(out, call.name);
        write_block(out, call.arguments);
    }
    write_block(out, message.content);
}

Message read_message(std::istream& in) {
    Message message;
    message.role = read_line(in);
    message.kind = read_line(in);
    message.file_id = read_line(in);
    message.pinned = read_line(in) == "1";
    message.tool_call_id = read_line(in);
    int calls = std::stoi(read_line(in));
    for (int i = 0; i < calls; ++i) {
        ToolCall call;
        call.id = read_line(in);
        call.name = read_line(in);
        call.arguments = read_block(in);
        message.tool_calls.push_back(std::move(call));
    }
    message.content = read_block(in);
    return message;
}

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 12) return false;
    for (char c : id) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string trim_copy(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

std::string one_line(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == ' ') {
            if (!space && !out.empty()) out.push_back(' ');
            space = true;
        } else {
            out.push_back(c);
            space = false;
        }
    }
    return out;
}

}  // namespace

std::string json_field(const Json* node) {
    if (!node) return {};
    if (node->type == Json::Type::String) return node->str;
    if (node->type == Json::Type::Number) {
        std::ostringstream raw;
        raw << node->num;
        return raw.str();
    }
    return {};
}

int count_tokens(const std::string& text) {
    if (text.empty()) return 0;
    int units = 0;
    for (unsigned char c : text) units += c < 128 ? 1 : 2;
    return std::max(1, (units + 3) / 4);
}

int count_messages(const std::vector<Message>& messages) {
    int total = 0;
    for (const auto& message : messages) {
        total += 4 + count_tokens(message.content);
        for (const auto& call : message.tool_calls) total += count_tokens(call.name) + count_tokens(call.arguments);
    }
    return total;
}

std::string format_pointer(const std::string& file_id, const std::string& summary) {
    std::string text = one_line(summary.empty() ? "earlier dialog" : summary);
    while (!text.empty() && (text.back() == '.' || text.back() == ';' || text.back() == ' ')) text.pop_back();
    std::string lower = utf8_lower(text);
    if (lower.find(kClause) == std::string::npos) text += std::string("; ") + kClause;
    return "file " + file_id + ": " + text;
}

std::string extractive_summary(const std::vector<Message>& messages) {
    std::string user_text;
    std::string all;
    bool all_pointers = !messages.empty();
    std::vector<std::string> child_ids;
    for (const auto& message : messages) {
        all += message.content + " ";
        if (message.role == "user") user_text += message.content + " ";
        if (message.kind != "pointer") all_pointers = false;
        if (!message.file_id.empty()) child_ids.push_back(message.file_id);
    }
    auto words = top_keywords(user_text.empty() ? all : user_text, 2);
    std::string sentence;
    if (all_pointers) {
        std::string ids;
        for (size_t i = 0; i < child_ids.size(); ++i) {
            if (i) ids += ", ";
            ids += child_ids[i];
        }
        sentence = ids.empty() ? "index of earlier pointers" : "index of files " + ids;
        if (!words.empty()) sentence += ": " + join_topics(words);
    } else if (!user_text.empty() && !words.empty()) {
        sentence = "user asked about " + join_topics(words);
    } else if (!words.empty()) {
        sentence = "dialog covered " + join_topics(words);
    } else {
        sentence = "earlier dialog";
    }
    if (sentence.size() > 180) sentence.resize(179);
    return one_line(sentence);
}

std::vector<Directive> parse_directives(const std::string& text) {
    std::vector<Directive> found;
    auto add = [&](Directive d) {
        for (const auto& have : found) {
            if (have.kind == d.kind && have.file_id == d.file_id && have.query == d.query) return;
        }
        found.push_back(std::move(d));
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 11, "[[ctx:read:") == 0) {
            size_t end = text.find("]]", i + 11);
            if (end == std::string::npos) continue;
            Directive d;
            d.kind = "read";
            d.file_id = text.substr(i + 11, end - (i + 11));
            if (valid_id(d.file_id)) add(std::move(d));
            i = end + 1;
        } else if (text.compare(i, 13, "[[ctx:search:") == 0) {
            size_t end = text.find("]]", i + 13);
            if (end == std::string::npos) continue;
            Directive d;
            d.kind = "search";
            d.query = one_line(text.substr(i + 13, end - (i + 13)));
            if (!d.query.empty()) add(std::move(d));
            i = end + 1;
        }
    }
    auto find_attr = [&](const std::string& tag, const std::string& attr) {
        size_t pos = 0;
        while ((pos = text.find(tag, pos)) != std::string::npos) {
            size_t end = text.find('>', pos);
            if (end == std::string::npos) break;
            std::string chunk = text.substr(pos, end - pos);
            size_t at = chunk.find(attr + "=\"");
            if (at != std::string::npos) {
                at += attr.size() + 2;
                size_t q = chunk.find('"', at);
                if (q != std::string::npos) {
                    Directive d;
                    if (attr == "id") {
                        d.kind = "read";
                        d.file_id = chunk.substr(at, q - at);
                        if (valid_id(d.file_id)) add(std::move(d));
                    } else {
                        d.kind = "search";
                        d.query = one_line(chunk.substr(at, q - at));
                        if (!d.query.empty()) add(std::move(d));
                    }
                }
            }
            pos = end + 1;
        }
    };
    find_attr("<ctx-read", "id");
    find_attr("<ctx-search", "q");
    return found;
}

std::string strip_directives(const std::string& text) {
    std::string out = text;
    auto cut = [&](const std::string& open, const std::string& close) {
        size_t pos = 0;
        while ((pos = out.find(open, pos)) != std::string::npos) {
            size_t end = out.find(close, pos + open.size());
            if (end == std::string::npos) break;
            out.erase(pos, end + close.size() - pos);
        }
    };
    cut("[[ctx:read:", "]]");
    cut("[[ctx:search:", "]]");
    cut("<ctx-read", ">");
    cut("<ctx-search", ">");
    return one_line(out);
}

std::string render_segment_text(const Segment& segment) {
    std::ostringstream out;
    out << "BEGIN FILE " << segment.id << "\nsummary: " << segment.summary << "\n---\n";
    for (const auto& message : segment.messages) out << message.role << ": " << message.content << "\n";
    out << "END FILE " << segment.id;
    return out.str();
}

Store::Store(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_ / "segments");
    load_manifest();
}

void Store::load_manifest() {
    std::ifstream in(root_ / "manifest.txt");
    if (!in) {
        next_id_ = 1;
        return;
    }
    in >> next_id_;
    if (next_id_ < 1) next_id_ = 1;
}

void Store::save_manifest() const {
    std::ofstream out(root_ / "manifest.txt", std::ios::trunc);
    out << next_id_ << "\n";
}

std::string Store::write_segment(Segment segment) {
    if (segment.id.empty()) {
        segment.id = std::to_string(next_id_++);
        save_manifest();
    }
    if (!valid_id(segment.id)) throw std::runtime_error("invalid file id");
    auto path = root_ / "segments" / (segment.id + ".txt");
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write segment");
        out << "CB1\n";
        write_u(out, segment.id);
        write_u(out, segment.kind);
        write_block(out, segment.summary);
        write_u(out, std::to_string(segment.messages.size()));
        for (const auto& message : segment.messages) write_message(out, message);
    }
    std::filesystem::remove(path);
    std::filesystem::rename(tmp, path);
    return segment.id;
}

Segment Store::read_segment(const std::string& id) const {
    if (!valid_id(id)) throw std::runtime_error("invalid file id");
    auto path = root_ / "segments" / (id + ".txt");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("file " + id + " not found");
    if (read_line(in) != "CB1") throw std::runtime_error("bad segment file");
    Segment segment;
    segment.id = read_line(in);
    segment.kind = read_line(in);
    segment.summary = read_block(in);
    int count = std::stoi(read_line(in));
    for (int i = 0; i < count; ++i) segment.messages.push_back(read_message(in));
    return segment;
}

std::vector<Segment> Store::list_segments() const {
    std::vector<Segment> all;
    if (!std::filesystem::exists(root_ / "segments")) return all;
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root_ / "segments")) {
        if (entry.path().extension() == ".txt") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
        return std::stoi(a.stem().string()) < std::stoi(b.stem().string());
    });
    for (const auto& path : paths) all.push_back(read_segment(path.stem().string()));
    return all;
}

void Store::save_session(const std::vector<Message>& messages) const {
    auto path = root_ / "session.txt";
    auto tmp = root_ / "session.txt.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << messages.size() << "\n";
        for (const auto& message : messages) write_message(out, message);
    }
    std::filesystem::remove(path);
    std::filesystem::rename(tmp, path);
}

std::vector<Message> Store::load_session() const {
    std::ifstream in(root_ / "session.txt", std::ios::binary);
    if (!in) return {};
    int count = std::stoi(read_line(in));
    std::vector<Message> messages;
    for (int i = 0; i < count; ++i) messages.push_back(read_message(in));
    return messages;
}

Bridge::Bridge(Store store_in, Config config_in) : store(std::move(store_in)), config(std::move(config_in)) {
    messages = store.load_session();
    if (messages.empty()) {
        seed();
        save();
    }
}

void Bridge::seed() {
    if (!config.inject_protocol) return;
    Message protocol;
    protocol.role = "system";
    protocol.content = kProtocol;
    protocol.kind = "protocol";
    protocol.pinned = true;
    messages.push_back(std::move(protocol));
}

int Bridge::prompt_budget() const { return std::max(1, config.max_context_tokens - config.reserve_tokens); }

int Bridge::threshold_tokens() const {
    return std::max(1, static_cast<int>(prompt_budget() * config.compact_threshold));
}

int Bridge::prompt_tokens() const { return count_messages(messages); }

std::vector<Message> Bridge::pointers() const {
    std::vector<Message> out;
    for (const auto& message : messages) {
        if (message.kind == "pointer") out.push_back(message);
    }
    return out;
}

void Bridge::split(std::vector<Message>& pinned, std::vector<Message>& head, std::vector<Message>& tail) const {
    size_t i = 0;
    while (i < messages.size() && messages[i].pinned) pinned.push_back(messages[i++]);
    std::vector<Message> rest(messages.begin() + static_cast<std::ptrdiff_t>(i), messages.end());
    int keep = std::max(0, config.min_keep_messages);
    if (keep == 0 || static_cast<int>(rest.size()) <= keep) {
        tail = std::move(rest);
        return;
    }
    head.assign(rest.begin(), rest.end() - keep);
    tail.assign(rest.end() - keep, rest.end());
}

bool Bridge::dump_once(std::vector<TurnEvent>* events, bool relax_if_needed, bool force) {
    std::vector<Message> pinned, head, tail;
    split(pinned, head, tail);
    bool over = prompt_tokens() >= threshold_tokens();
    if (head.empty() && relax_if_needed && tail.size() > 1 && (over || force)) {
        head.assign(tail.begin(), tail.end() - 1);
        tail = {tail.back()};
    }
    if (head.empty()) return false;
    int limit = config.dump_chunk_tokens > 0 ? config.dump_chunk_tokens : std::max(32, prompt_budget() * 45 / 100);
    std::vector<Message> chunk;
    int used = 0;
    for (const auto& message : head) {
        int cost = count_messages({message});
        if (!chunk.empty() && (used + cost > limit || static_cast<int>(chunk.size()) >= config.max_chunk_messages)) break;
        chunk.push_back(message);
        used += cost;
        if (static_cast<int>(chunk.size()) >= config.max_chunk_messages || used >= limit) break;
    }
    if (chunk.empty()) return false;
    std::vector<Message> leftover(head.begin() + static_cast<std::ptrdiff_t>(chunk.size()), head.end());
    std::string summary = extractive_summary(chunk);
    Segment segment;
    segment.kind = "dialog";
    bool only_pointers = !chunk.empty();
    for (const auto& message : chunk) {
        if (message.kind != "pointer") only_pointers = false;
        if (!message.file_id.empty()) {
            // child ids live in the summary text produced above
        }
    }
    if (only_pointers) segment.kind = "index";
    segment.summary = summary;
    segment.messages = chunk;
    std::string file_id;
    try {
        file_id = store.write_segment(segment);
    } catch (const std::exception& ex) {
        warning = ex.what();
        if (events) events->push_back({"warning", "", warning});
        return false;
    }
    Message pointer;
    pointer.role = "system";
    pointer.kind = "pointer";
    pointer.file_id = file_id;
    pointer.content = format_pointer(file_id, summary);
    messages = pinned;
    messages.push_back(pointer);
    messages.insert(messages.end(), leftover.begin(), leftover.end());
    messages.insert(messages.end(), tail.begin(), tail.end());
    warning.clear();
    if (events) events->push_back({"compacted", file_id, pointer.content});
    return true;
}

void Bridge::ensure_budget(std::vector<TurnEvent>* events) {
    for (int i = 0; i < 32; ++i) {
        if (prompt_tokens() < threshold_tokens()) {
            warning.clear();
            return;
        }
        if (!dump_once(events, true)) {
            warning = "Окно всё ещё выше порога: закреплённые или последние реплики не влезают.";
            if (events) events->push_back({"warning", "", warning});
            return;
        }
    }
}

std::string Bridge::read_for_model(const std::string& file_id) const {
    Segment segment = store.read_segment(file_id);
    std::string text = render_segment_text(segment);
    int ceiling = std::max(64, prompt_budget() / 2);
    if (count_tokens(text) <= ceiling) return text;
    size_t chars = static_cast<size_t>(ceiling) * 4;
    if (chars < text.size()) text.resize(chars);
    return text + "\n…[truncated] file " + file_id + " has the full original";
}

std::string Bridge::search_for_model(const std::string& query) const {
    auto needles = tokenize(query);
    if (needles.empty() && !query.empty() && valid_id(trim_copy(query))) needles.push_back(trim_copy(query));
    std::ostringstream out;
    int hits = 0;
    for (const auto& segment : store.list_segments()) {
        if (valid_id(trim_copy(query)) && segment.id == trim_copy(query)) {
            out << "file " << segment.id << ": " << segment.summary << "\n";
            ++hits;
            continue;
        }
        std::string blob = segment.summary;
        for (const auto& message : segment.messages) blob += " " + message.content;
        auto words = tokenize(blob);
        int score = 0;
        for (const auto& needle : needles) {
            for (const auto& word : words) {
                if (word == needle) ++score;
            }
        }
        if (score > 0) {
            out << "file " << segment.id << " (" << score << "): " << segment.summary << "\n";
            if (++hits >= 5) break;
        }
    }
    if (hits == 0) return "no matches";
    return "search hits:\n" + out.str();
}

void Bridge::save() { store.save_session(messages); }

TurnResult Bridge::run_turn(const std::string& user_text, const std::function<ModelReply(const std::vector<Message>&)>& ask) {
    TurnResult result;
    std::string text = trim_copy(user_text);
    if (text.empty()) {
        result.error = "Пустое сообщение.";
        return result;
    }
    Message user;
    user.role = "user";
    user.content = text;
    messages.push_back(user);
    ensure_budget(&result.events);
    std::vector<Message> working = messages;
    ModelReply reply;
    for (int hop = 0; hop < config.max_tool_hops; ++hop) {
        reply = ask(working);
        if (!reply.error.empty()) {
            result.error = reply.error;
            save();
            return result;
        }
        auto directives = parse_directives(reply.text);
        if (reply.tool_calls.empty() && directives.empty()) break;
        Message assistant;
        assistant.role = "assistant";
        assistant.content = reply.text;
        assistant.tool_calls = reply.tool_calls;
        working.push_back(assistant);
        bool progressed = false;
        for (const auto& call : reply.tool_calls) {
            std::string payload;
            try {
                if (call.name == "read_context_file") {
                    std::string id;
                    try {
                        Json args = parse_json(call.arguments.empty() ? "{}" : call.arguments);
                        if (auto* node = args.get("file_id")) id = json_field(node);
                    } catch (...) {
                        id = call.arguments;
                    }
                    payload = read_for_model(id);
                    result.events.push_back({"read", id, ""});
                } else if (call.name == "search_context") {
                    std::string query = call.arguments;
                    try {
                        Json args = parse_json(call.arguments.empty() ? "{}" : call.arguments);
                        if (auto* node = args.get("query")) query = json_field(node);
                    } catch (...) {
                    }
                    payload = search_for_model(query);
                    result.events.push_back({"search", "", query});
                } else {
                    payload = "error: unknown tool " + call.name;
                }
            } catch (const std::exception& ex) {
                payload = std::string("error: ") + ex.what();
            }
            Message tool;
            tool.role = "tool";
            tool.content = payload;
            tool.tool_call_id = call.id;
            working.push_back(tool);
            progressed = true;
        }
        for (const auto& directive : directives) {
            bool seen = false;
            for (const auto& event : result.events) {
                if (directive.kind == "read" && event.type == "read" && event.file_id == directive.file_id) seen = true;
            }
            if (seen) continue;
            std::string payload;
            try {
                if (directive.kind == "read") {
                    payload = read_for_model(directive.file_id);
                    result.events.push_back({"read", directive.file_id, ""});
                } else {
                    payload = search_for_model(directive.query);
                    result.events.push_back({"search", "", directive.query});
                }
            } catch (const std::exception& ex) {
                payload = std::string("error: ") + ex.what();
            }
            Message note;
            note.role = "system";
            note.kind = "retrieval";
            note.content = payload;
            working.push_back(note);
            progressed = true;
        }
        if (!progressed) break;
    }
    std::string visible = strip_directives(reply.text);
    if (visible.empty()) visible = result.events.empty() ? "" : "Мост прочитал архив, но модель не дала итоговый ответ.";
    Message assistant;
    assistant.role = "assistant";
    assistant.content = visible;
    messages.push_back(assistant);
    ensure_budget(&result.events);
    save();
    result.reply = visible;
    return result;
}

ModelReply local_reply(const std::vector<Message>& messages) {
    std::string user;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "user") {
            user = it->content;
            break;
        }
    }
    auto has_file = [&] {
        for (const auto& message : messages) {
            if (message.content.find("BEGIN FILE ") != std::string::npos) return true;
        }
        return false;
    };
    if (has_file()) {
        std::string body;
        for (const auto& message : messages) {
            if (message.content.find("BEGIN FILE ") != std::string::npos) body = message.content;
        }
        std::string line;
        std::istringstream in(body);
        std::string row;
        while (std::getline(in, row)) {
            if (row.rfind("user:", 0) == 0 || row.rfind("assistant:", 0) == 0) {
                line = row;
                break;
            }
        }
        if (line.size() > 280) line.resize(277);
        ModelReply reply;
        bool ru = user.find("а") != std::string::npos || utf8_lower(user).find("перечитай") != std::string::npos;
        reply.text = (ru ? "В оригинале так: " : "The original says: ") + line;
        return reply;
    }
    std::string lower = utf8_lower(user);
    bool recall = lower.find("перечитай") != std::string::npos || lower.find("прочитай") != std::string::npos ||
                  lower.find("оригинал") != std::string::npos || lower.find("re-read") != std::string::npos ||
                  lower.find("reread") != std::string::npos;
    if (recall) {
        std::string id;
        std::string digits;
        for (char c : user) {
            if (c >= '0' && c <= '9') digits.push_back(c);
            else if (!digits.empty()) {
                id = digits;
                break;
            }
        }
        if (id.empty()) id = digits;
        if (!id.empty()) {
            ModelReply reply;
            reply.text = "Открываю архив. [[ctx:read:" + id + "]]";
            return reply;
        }
    }
    ModelReply reply;
    std::string snippet = one_line(user);
    if (snippet.size() > 180) snippet.resize(177);
    bool ru = false;
    for (unsigned char c : user) {
        if (c >= 128) ru = true;
    }
    if (ru) reply.text = "В текущем окне только это: «" + snippet + "».";
    else reply.text = "The current window only has: \"" + snippet + "\".";
    int pointers = 0;
    std::string listed;
    for (const auto& message : messages) {
        if (message.kind == "pointer") {
            if (pointers) listed += ", ";
            listed += "file " + message.file_id;
            ++pointers;
        }
    }
    if (pointers) {
        if (ru) reply.text += " Старое свёрнуто в указатели: " + listed + ".";
        else reply.text += " Older context is stored behind " + listed + ".";
    }
    return reply;
}

std::string endpoint_for(AdapterKind kind, const std::string& base_url) {
    std::string base = trim_copy(base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (kind == AdapterKind::Ollama) {
        if (base.size() >= 3 && base.substr(base.size() - 3) == "/v1") base.resize(base.size() - 3);
        if (base.empty()) base = "http://127.0.0.1:11434";
        return base + "/api/chat";
    }
    if (base.empty()) {
        if (kind == AdapterKind::LlamaCpp) base = "http://127.0.0.1:8080/v1";
        else if (kind == AdapterKind::HuggingFace) base = "https://router.huggingface.co/v1";
        else base = "https://api.openai.com/v1";
    }
    if (base.size() < 3 || base.substr(base.size() - 3) != "/v1") base += "/v1";
    return base + "/chat/completions";
}

std::string request_body(AdapterKind kind, const std::string& model, const std::vector<Message>& messages, bool tools) {
    std::ostringstream out;
    out << "{\"model\":\"" << json_escape(model) << "\",\"messages\":[";
    bool first = true;
    for (const auto& message : messages) {
        if (message.kind == "note") continue;
        if (!first) out << ',';
        first = false;
        std::string role = message.role.empty() ? "user" : message.role;
        out << "{\"role\":\"" << json_escape(role) << "\",\"content\":\"" << json_escape(message.content) << "\"";
        if (role == "tool") {
            out << ",\"tool_call_id\":\"" << json_escape(message.tool_call_id) << "\"";
        }
        if (!message.tool_calls.empty() && kind != AdapterKind::Ollama) {
            out << ",\"tool_calls\":[";
            for (size_t i = 0; i < message.tool_calls.size(); ++i) {
                if (i) out << ',';
                const auto& call = message.tool_calls[i];
                out << "{\"id\":\"" << json_escape(call.id) << "\",\"type\":\"function\",\"function\":{\"name\":\""
                    << json_escape(call.name) << "\",\"arguments\":\"" << json_escape(call.arguments) << "\"}}";
            }
            out << ']';
        }
        if (!message.tool_calls.empty() && kind == AdapterKind::Ollama) {
            out << ",\"tool_calls\":[";
            for (size_t i = 0; i < message.tool_calls.size(); ++i) {
                if (i) out << ',';
                const auto& call = message.tool_calls[i];
                std::string args = call.arguments.empty() ? "{}" : call.arguments;
                out << "{\"function\":{\"name\":\"" << json_escape(call.name) << "\",\"arguments\":" << args << "}}";
            }
            out << ']';
        }
        out << '}';
    }
    out << ']';
    if (kind == AdapterKind::Ollama) out << ",\"stream\":false";
    if (tools && kind != AdapterKind::Local) {
        out << ",\"tools\":["
               "{\"type\":\"function\",\"function\":{\"name\":\"read_context_file\",\"description\":\"Read a dumped context file by numeric id.\","
               "\"parameters\":{\"type\":\"object\",\"properties\":{\"file_id\":{\"type\":\"string\"}},\"required\":[\"file_id\"]}}},"
               "{\"type\":\"function\",\"function\":{\"name\":\"search_context\",\"description\":\"Keyword search over dumped context files.\","
               "\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}}}"
               "]";
    }
    out << '}';
    return out.str();
}

ModelReply parse_response(AdapterKind kind, const std::string& body) {
    Json root;
    try {
        root = parse_json(body);
    } catch (const std::exception&) {
        throw std::runtime_error("Модель вернула не JSON: " + body.substr(0, std::min<size_t>(body.size(), 240)));
    }
    if (auto* err = root.get("error")) {
        std::string message;
        if (err->type == Json::Type::Object) message = json_field(err->get("message"));
        if (message.empty() && err->type == Json::Type::String) message = err->str;
        if (message.empty()) message = "request failed";
        throw std::runtime_error(message);
    }
    const Json* message = nullptr;
    if (kind == AdapterKind::Ollama) message = root.get("message");
    else if (auto* choices = root.get("choices"); choices && !choices->arr.empty()) message = choices->arr[0].get("message");
    if (!message) throw std::runtime_error("В ответе модели нет message.");
    ModelReply reply;
    if (auto* content = message->get("content"); content && content->type == Json::Type::String) reply.text = content->str;
    if (auto* calls = message->get("tool_calls"); calls && calls->type == Json::Type::Array) {
        int n = 0;
        for (const auto& call : calls->arr) {
            const Json* fn = call.get("function");
            if (!fn) fn = &call;
            ToolCall tool;
            tool.id = json_field(call.get("id"));
            if (tool.id.empty()) tool.id = "call_" + std::to_string(n);
            tool.name = json_field(fn->get("name"));
            if (auto* args = fn->get("arguments")) {
                if (args->type == Json::Type::String) tool.arguments = args->str;
                else if (args->type == Json::Type::Object) {
                    std::ostringstream raw;
                    raw << '{';
                    for (size_t i = 0; i < args->obj.size(); ++i) {
                        if (i) raw << ',';
                        raw << '"' << json_escape(args->obj[i].first) << "\":";
                        if (args->obj[i].second.type == Json::Type::String) raw << '"' << json_escape(args->obj[i].second.str) << '"';
                        else if (args->obj[i].second.type == Json::Type::Number) raw << args->obj[i].second.num;
                        else raw << "null";
                    }
                    raw << '}';
                    tool.arguments = raw.str();
                }
            }
            reply.tool_calls.push_back(std::move(tool));
            ++n;
        }
    }
    return reply;
}
