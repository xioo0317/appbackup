#include "meta.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common.h"
#include "fsops.h"

namespace ozj {

const char* const kDataTypeUser = "user";
const char* const kDataTypeUserDe = "user_de";
const char* const kDataTypeData = "data";
const char* const kDataTypeObb = "obb";
const char* const kDataTypeMedia = "media";
const char* const kDataTypeApk = "apk";

std::vector<std::string> all_data_types() {
    return {kDataTypeUser, kDataTypeUserDe, kDataTypeData, kDataTypeObb, kDataTypeMedia, kDataTypeApk};
}

std::string data_type_dir_template(const std::string& type) {
    if (type == kDataTypeUser) return "/data/user/%d";
    if (type == kDataTypeUserDe) return "/data/user_de/%d";
    if (type == kDataTypeData) return "/data/media/%d/Android/data";
    if (type == kDataTypeObb) return "/data/media/%d/Android/obb";
    if (type == kDataTypeMedia) return "/data/media/%d/Android/media";
    return std::string();
}

bool data_type_is_external(const std::string& type) {
    return type == kDataTypeData || type == kDataTypeObb || type == kDataTypeMedia;
}

namespace {

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    std::string as_string() const {
        if (kind == Kind::String) return str;
        if (kind == Kind::Number) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num));
            return buf;
        }
        if (kind == Kind::Bool) return b ? "true" : "false";
        return std::string();
    }
    long long as_int() const {
        if (kind == Kind::Number) return static_cast<long long>(num);
        if (kind == Kind::String) return atoll(str.c_str());
        return 0;
    }
    bool as_bool() const {
        if (kind == Kind::Bool) return b;
        if (kind == Kind::Number) return num != 0;
        return false;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(JsonValue* out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }

    bool eat(char c) {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == c) {
            i_++;
            return true;
        }
        return false;
    }

    bool parse_value(JsonValue* out) {
        skip_ws();
        if (i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out->kind = JsonValue::Kind::String;
            return parse_string(&out->str);
        }
        if (c == 't' || c == 'f') {
            if (s_.compare(i_, 4, "true") == 0) {
                out->kind = JsonValue::Kind::Bool;
                out->b = true;
                i_ += 4;
                return true;
            }
            if (s_.compare(i_, 5, "false") == 0) {
                out->kind = JsonValue::Kind::Bool;
                out->b = false;
                i_ += 5;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (s_.compare(i_, 4, "null") == 0) {
                out->kind = JsonValue::Kind::Null;
                i_ += 4;
                return true;
            }
            return false;
        }
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
        while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
                                 s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
            i_++;
        }
        if (i_ == start) return false;
        out->kind = JsonValue::Kind::Number;
        out->num = atof(s_.substr(start, i_ - start).c_str());
        return true;
    }

    bool parse_string(std::string* out) {
        if (!eat('"')) return false;
        out->clear();
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case 'n': *out += '\n'; break;
                case 't': *out += '\t'; break;
                case 'r': *out += '\r'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    std::string hex = s_.substr(i_, 4);
                    i_ += 4;
                    unsigned int cp = static_cast<unsigned int>(strtoul(hex.c_str(), nullptr, 16));
                    if (cp < 0x80) {
                        *out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        *out += static_cast<char>(0xC0 | (cp >> 6));
                        *out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        *out += static_cast<char>(0xE0 | (cp >> 12));
                        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        *out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: *out += e;
            }
        }
        return false;
    }

    bool parse_array(JsonValue* out) {
        if (!eat('[')) return false;
        out->kind = JsonValue::Kind::Array;
        skip_ws();
        if (eat(']')) return true;
        while (true) {
            JsonValue v;
            if (!parse_value(&v)) return false;
            out->arr.push_back(std::move(v));
            skip_ws();
            if (eat(',')) continue;
            if (eat(']')) return true;
            return false;
        }
    }

    bool parse_object(JsonValue* out) {
        if (!eat('{')) return false;
        out->kind = JsonValue::Kind::Object;
        skip_ws();
        if (eat('}')) return true;
        while (true) {
            std::string key;
            skip_ws();
            if (!parse_string(&key)) return false;
            if (!eat(':')) return false;
            JsonValue v;
            if (!parse_value(&v)) return false;
            out->obj.emplace_back(key, std::move(v));
            skip_ws();
            if (eat(',')) continue;
            if (eat('}')) return true;
            return false;
        }
    }
};

}  // namespace

std::string backup_meta_to_json(const BackupMeta& meta) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"tool\": \"" << json_escape(meta.tool) << "\",\n";
    os << "  \"tool_version\": \"" << json_escape(meta.tool_version) << "\",\n";
    os << "  \"pkg\": \"" << json_escape(meta.pkg) << "\",\n";
    os << "  \"user\": " << meta.user << ",\n";
    os << "  \"uid\": " << meta.uid << ",\n";
    os << "  \"gid\": " << meta.gid << ",\n";
    os << "  \"created_at\": \"" << json_escape(meta.created_at) << "\",\n";
    os << "  \"device\": \"" << json_escape(meta.device) << "\",\n";
    os << "  \"sdk\": \"" << json_escape(meta.sdk) << "\",\n";
    os << "  \"compression\": \"" << json_escape(meta.compression) << "\",\n";
    os << "  \"entries\": [\n";
    for (size_t i = 0; i < meta.entries.size(); i++) {
        const auto& e = meta.entries[i];
        os << "    {\n";
        os << "      \"type\": \"" << json_escape(e.type) << "\",\n";
        os << "      \"src_path\": \"" << json_escape(e.src_path) << "\",\n";
        os << "      \"arc_name\": \"" << json_escape(e.arc_name) << "\",\n";
        os << "      \"archive\": \"" << json_escape(e.archive) << "\",\n";
        os << "      \"context\": \"" << json_escape(e.context) << "\",\n";
        os << "      \"gid\": " << e.gid << ",\n";
        os << "      \"uid\": " << e.uid << ",\n";
        os << "      \"exists\": " << (e.exists ? "true" : "false") << ",\n";
        os << "      \"files\": " << e.files << ",\n";
        os << "      \"bytes\": " << e.bytes << "\n";
        os << "    }" << (i + 1 < meta.entries.size() ? "," : "") << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

bool backup_meta_from_json(const std::string& text, BackupMeta* meta) {
    JsonParser parser(text);
    JsonValue root;
    if (!parser.parse(&root) || root.kind != JsonValue::Kind::Object) {
        log_error("meta.json parse failed");
        return false;
    }
    if (auto v = root.get("tool")) meta->tool = v->as_string();
    if (auto v = root.get("tool_version")) meta->tool_version = v->as_string();
    if (auto v = root.get("pkg")) meta->pkg = v->as_string();
    if (auto v = root.get("user")) meta->user = static_cast<int>(v->as_int());
    if (auto v = root.get("uid")) meta->uid = static_cast<int>(v->as_int());
    if (auto v = root.get("gid")) meta->gid = static_cast<int>(v->as_int());
    if (auto v = root.get("created_at")) meta->created_at = v->as_string();
    if (auto v = root.get("device")) meta->device = v->as_string();
    if (auto v = root.get("sdk")) meta->sdk = v->as_string();
    if (auto v = root.get("compression")) meta->compression = v->as_string();
    if (auto arr = root.get("entries")) {
        if (arr->kind == JsonValue::Kind::Array) {
            for (const auto& item : arr->arr) {
                if (item.kind != JsonValue::Kind::Object) continue;
                DataEntry e;
                if (auto v = item.get("type")) e.type = v->as_string();
                if (auto v = item.get("src_path")) e.src_path = v->as_string();
                if (auto v = item.get("arc_name")) e.arc_name = v->as_string();
                if (auto v = item.get("archive")) e.archive = v->as_string();
                if (auto v = item.get("context")) e.context = v->as_string();
                if (auto v = item.get("gid")) e.gid = v->as_int();
                if (auto v = item.get("uid")) e.uid = v->as_int();
                if (auto v = item.get("exists")) e.exists = v->as_bool();
                if (auto v = item.get("files")) e.files = static_cast<uint64_t>(v->as_int());
                if (auto v = item.get("bytes")) e.bytes = static_cast<uint64_t>(v->as_int());
                meta->entries.push_back(e);
            }
        }
    }
    if (meta->pkg.empty()) {
        log_error("meta.json missing pkg");
        return false;
    }
    return true;
}

bool write_meta_file(const std::string& path, const BackupMeta& meta) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        log_error("cannot write %s", path.c_str());
        return false;
    }
    f << backup_meta_to_json(meta);
    f.close();
    return true;
}

bool read_meta_file(const std::string& path, BackupMeta* meta) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        log_error("cannot read %s", path.c_str());
        return false;
    }
    std::ostringstream os;
    os << f.rdbuf();
    return backup_meta_from_json(os.str(), meta);
}

std::string json_get_string(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return std::string();
    size_t colon = text.find(':', pos + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t q1 = text.find('"', colon);
    if (q1 == std::string::npos) return std::string();
    size_t q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();
    return text.substr(q1 + 1, q2 - q1 - 1);
}

}  // namespace ozj
