#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

// --- 1) 型コード定義 ---
enum class FieldType : uint8_t { UINT8, UINT16, UINT32, INT32, BLOB, BITFIELD };

// --- 2) フィールド記述子 ---
struct FieldDesc {
  std::string name;
  FieldType type;
  size_t size = 0;
  size_t offset = 0;
  size_t bitOffset = 0;
  uint8_t bitLength = 0;
};

// --- 3) ビット操作ユーティリティ ---
static uint64_t readBits(const std::vector<char>& buf, size_t bitOffset,
                         uint8_t bitWidth) {
  size_t byte0 = bitOffset / 8;
  size_t byte1 = (bitOffset + bitWidth - 1) / 8;
  uint64_t chunk = 0;
  std::memcpy(&chunk, buf.data() + byte0, byte1 - byte0 + 1);
  chunk >>= (bitOffset % 8);
  uint64_t mask = (bitWidth == 64 ? ~0ull : ((1ull << bitWidth) - 1));
  return chunk & mask;
}
static void writeBits(std::vector<char>& buf, size_t bitOffset,
                      uint8_t bitWidth, uint64_t value) {
  size_t byte0 = bitOffset / 8;
  size_t byte1 = (bitOffset + bitWidth - 1) / 8;
  uint8_t shift = bitOffset % 8;
  uint64_t mask = (bitWidth == 64 ? ~0ull : ((1ull << bitWidth) - 1));
  for (size_t b = byte0; b <= byte1; ++b) {
    uint8_t clearMask = ~(((mask << shift) >> ((b - byte0) * 8)) & 0xFF);
    buf[b] &= clearMask;
  }
  uint64_t chunk = (value & mask) << shift;
  std::memcpy(buf.data() + byte0, &chunk, byte1 - byte0 + 1);
}

// --- 4) スキーマクラス ---
class BinarySchema {
 public:
  std::vector<FieldDesc> fields;
  std::unordered_map<std::string, size_t> name2idx;
  size_t totalSize = 0;
  size_t totalBits = 0;

  void loadSchema(const nlohmann::ordered_json& schema) {
    size_t cursorBits = 0;
    for (auto& item : schema) {
      FieldDesc fd;
      fd.name = item["name"].get<std::string>();

      if (auto bitLength = item["bitLength"].get<uint8_t>();
          bitLength > 0 && bitLength <= 64) {
        fd.bitLength = bitLength;
      } else {
        throw std::runtime_error("Invalid bitLength for field: " + fd.name);
      }
      fd.type = FieldType::BITFIELD;
      fd.bitOffset = cursorBits;
      cursorBits += fd.bitLength;
      fd.size = (fd.bitLength + 7) / 8;
      fd.offset = fd.bitOffset / 8;
      fields.push_back(fd);
    }
    totalBits = cursorBits;
    totalSize = (totalBits + 7) / 8;
    name2idx.reserve(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
      name2idx[fields[i].name] = i;
    }
  }
};

// --- 5) レコードクラス ---
class DynamicRecord {
  const BinarySchema& schema;
  std::vector<char> buf;

 public:
  DynamicRecord(const BinarySchema& s) : schema(s), buf(s.totalSize, 0) {}

  // 一括読み込み
  void read(std::istream& is) { is.read(buf.data(), buf.size()); }

  // コピー取得
  template <typename T>
  T getValue(const std::string& name) const {
    static_assert(
        std::is_integral_v<T> || std::is_same_v<T, std::vector<uint8_t>>,
        "T must be integer or blob vector");
    auto it = schema.name2idx.find(name);
    if (it == schema.name2idx.end())
      throw std::out_of_range("Unknown field: " + name);
    const FieldDesc& fd = schema.fields[it->second];
    if constexpr (std::is_integral_v<T>) {
      uint64_t raw = 0;
      if (fd.type == FieldType::BITFIELD)
        raw = readBits(buf, fd.bitOffset, fd.bitLength);
      else
        std::memcpy(&raw, buf.data() + fd.offset, fd.size);
      return static_cast<T>(raw);
    } else {
      return std::vector<uint8_t>(
          reinterpret_cast<const uint8_t*>(buf.data() + fd.offset),
          reinterpret_cast<const uint8_t*>(buf.data() + fd.offset + fd.size));
    }
  }

  // 汎用整数取得
  uint64_t getInteger(const std::string& name) const {
    auto it = schema.name2idx.find(name);
    if (it == schema.name2idx.end())
      throw std::out_of_range("Unknown field: " + name);
    const FieldDesc& fd = schema.fields[it->second];
    uint64_t raw;
    if (fd.type == FieldType::BITFIELD)
      raw = readBits(buf, fd.bitOffset, fd.bitLength);
    else
      switch (fd.type) {
        case FieldType::UINT8:
          raw = *reinterpret_cast<const uint8_t*>(buf.data() + fd.offset);
          break;
        case FieldType::UINT16:
          raw = *reinterpret_cast<const uint16_t*>(buf.data() + fd.offset);
          break;
        case FieldType::UINT32:
          raw = *reinterpret_cast<const uint32_t*>(buf.data() + fd.offset);
          break;
        case FieldType::INT32:
          raw = static_cast<int64_t>(
              *reinterpret_cast<const int32_t*>(buf.data() + fd.offset));
          break;
        default:
          throw std::runtime_error("Field '" + name +
                                   "' is not an integer type");
      }
    return raw;
  }

  // 汎用書き込み via uint64_t または blob
  void setValue(const std::string& name, uint64_t value) {
    auto it = schema.name2idx.find(name);
    if (it == schema.name2idx.end())
      throw std::out_of_range("Unknown field: " + name);
    const FieldDesc& fd = schema.fields[it->second];
    if (fd.type == FieldType::BITFIELD)
      writeBits(buf, fd.bitOffset, fd.bitLength, value);
    else
      switch (fd.type) {
        case FieldType::UINT8: {
          uint8_t v = static_cast<uint8_t>(value);
          std::memcpy(buf.data() + fd.offset, &v, 1);
        } break;
        case FieldType::UINT16: {
          uint16_t v = static_cast<uint16_t>(value);
          std::memcpy(buf.data() + fd.offset, &v, 2);
        } break;
        case FieldType::UINT32: {
          uint32_t v = static_cast<uint32_t>(value);
          std::memcpy(buf.data() + fd.offset, &v, 4);
        } break;
        case FieldType::INT32: {
          int32_t v = static_cast<int32_t>(value);
          std::memcpy(buf.data() + fd.offset, &v, 4);
        } break;
        default:
          throw std::runtime_error("Field '" + name +
                                   "' is not an integer type");
      }
  }
  void setValue(const std::string& name, const std::vector<uint8_t>& data) {
    auto it = schema.name2idx.find(name);
    if (it == schema.name2idx.end())
      throw std::out_of_range("Unknown field: " + name);
    const FieldDesc& fd = schema.fields[it->second];
    if (fd.type != FieldType::BLOB)
      throw std::runtime_error("Field '" + name + "' is not a blob field");
    size_t len = std::min(data.size(), fd.size);
    std::memcpy(buf.data() + fd.offset, data.data(), len);
    if (len < fd.size)
      std::memset(buf.data() + fd.offset + len, 0, fd.size - len);
  }

  // --- 6) operator[] で get/set ---
  struct FieldProxy {
    DynamicRecord* rec;
    std::string name;
    operator uint64_t() const { return rec->getInteger(name); }
    operator std::vector<uint8_t>() const {
      return rec->getValue<std::vector<uint8_t>>(name);
    }
    FieldProxy& operator=(uint64_t v) {
      rec->setValue(name, v);
      return *this;
    }
    FieldProxy& operator=(const std::vector<uint8_t>& v) {
      rec->setValue(name, v);
      return *this;
    }
  };
  FieldProxy operator[](const std::string& name) { return {this, name}; }
  FieldProxy operator[](const std::string& name) const {
    return {const_cast<DynamicRecord*>(this), name};
  }
  // --- 7) バッファをストリームに書き出すメソッド ---
  void write(std::ostream& os) const { os.write(buf.data(), buf.size()); }
  void dump(std::ostream& os) const {
    for (auto& byte : buf) {
      os << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)byte
         << ' ';
    }
    os << std::dec;
  }
};

// --- 使用例 ---
int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <schema.json>\n";
    return 1;
  }
  std::ifstream ifs(argv[1]);
  if (!ifs) {
    std::cerr << "Error: could not open " << argv[1] << "\n";
    return 1;
  }
  nlohmann::json schemaJson;
  ifs >> schemaJson;

  BinarySchema schema;
  schema.loadSchema(schemaJson);

  DynamicRecord rec(schema);

  constexpr uint8_t VERSION{1};
  constexpr uint64_t MAGIC{0x123456789abcdeull};
  constexpr uint32_t LENGTH{0x1357};
  constexpr uint16_t HEADER_LENGTH{0x48};
  constexpr uint16_t TYPE{0xab};

  // operator[] を使ってフィールドに値を設定
  rec["version"] = VERSION;              // 8bit
  rec["magic"] = MAGIC;                  // 56bit
  rec["length"] = LENGTH;                // 32bit
  rec["header_length"] = HEADER_LENGTH;  // 16bit
  rec["type"] = TYPE;                    // 16bit

  // バイナリをファイルに書き出し
  std::ofstream ofs("header.bin", std::ios::binary);
  if (!ofs) {
    std::cerr << "Error: could not open header.bin for writing\n";
    return 1;
  }
  rec.write(ofs);
  ofs.close();
  std::cout << "Header was encoded and written to header.bin\n";

  DynamicRecord rec2(schema);
  std::ifstream ifs2("header.bin", std::ios::binary);
  if (!ifs2) {
    std::cerr << "Error: could not open header.bin for reading\n";
    return 1;
  }
  rec2.read(ifs2);
  ifs2.close();
  std::cout << "Header was read and decoded from header.bin\n";

  // operator[] を使ってフィールドから値を取得
  std::cout << std::hex;
  std::cout << "Version:       0x" << rec2["version"] << "\n";
  std::cout << "Magic:         0x" << rec2["magic"] << "\n";
  std::cout << "Length:        0x" << rec2["length"] << "\n";
  std::cout << "Header Length: 0x" << rec2["header_length"] << "\n";
  std::cout << "Type:          0x" << rec2["type"] << "\n";
  std::cout << std::dec;

  assert(rec2["version"] == VERSION);
  assert(rec2["magic"] == MAGIC);
  assert(rec2["length"] == LENGTH);
  assert(rec2["header_length"] == HEADER_LENGTH);
  assert(rec2["type"] == TYPE);
  std::cout << "All values match!\n";

  return 0;
}
