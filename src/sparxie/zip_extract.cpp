#include "src/sparxie/zip_extract.h"

extern "C" {
#include "third-party/puff/puff.h"
}

#include <fstream>
#include <vector>

namespace sparxie::zip {
  namespace {
    constexpr std::uint32_t kEocd = 0x06054b50u;
    constexpr std::uint32_t kCentral = 0x02014b50u;
    constexpr std::uint32_t kLocal = 0x04034b50u;
    constexpr std::uint64_t kMaxArchive = 2ull << 30;
    constexpr std::uint64_t kMaxEntry = 1ull << 30;
    constexpr std::uint64_t kMaxTotal = 2ull << 30;
    constexpr unsigned kMaxEntries = 20000;
    const char kSlash = static_cast<char>(92);

    Outcome
    fail(std::string code, std::string message) {
      Outcome out;
      out.error_code = std::move(code);
      out.error = std::move(message);
      return out;
    }

    std::uint32_t
    crc32_of(const std::uint8_t *data, std::size_t size) {
      std::uint32_t crc = 0xffffffffu;
      for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
      }
      return ~crc;
    }

    struct Buf {
      std::vector<std::uint8_t> b;
      bool
      u16(std::size_t at, std::uint16_t &v) const {
        if (at + 2 > b.size()) return false;
        v = std::uint16_t(b[at] | (b[at + 1] << 8));
        return true;
      }
      bool
      u32(std::size_t at, std::uint32_t &v) const {
        if (at + 4 > b.size()) return false;
        v = std::uint32_t(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24));
        return true;
      }
      bool
      u64(std::size_t at, std::uint64_t &v) const {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(at, lo) || !u32(at + 4, hi)) return false;
        v = (std::uint64_t(hi) << 32) | lo;
        return true;
      }
    };

    bool
    load_file(const fs::path &archive, Buf &buf, Outcome &err) {
      std::error_code ec;
      if (!fs::is_regular_file(archive, ec)) {
        err = fail("not_found", "The package file is not there");
        return false;
      }
      const auto size = fs::file_size(archive, ec);
      if (ec || size > kMaxArchive) {
        err = fail("bad_zip", "The package is too large to install");
        return false;
      }
      std::ifstream in(archive, std::ios::binary);
      if (!in) {
        err = fail("not_found", "The package file could not be read");
        return false;
      }
      buf.b.resize(static_cast<std::size_t>(size));
      if (size && !in.read(reinterpret_cast<char *>(buf.b.data()), static_cast<std::streamsize>(size))) {
        err = fail("bad_zip", "The package could not be read");
        return false;
      }
      return true;
    }

    bool
    find_eocd(const Buf &buf, std::size_t &at) {
      if (buf.b.size() < 22) return false;
      const std::size_t begin = buf.b.size() > (65535u + 22u) ? buf.b.size() - (65535u + 22u) : 0;
      std::size_t i = buf.b.size() - 22;
      for (;;) {
        std::uint32_t sig = 0;
        std::uint16_t comment = 0;
        if (buf.u32(i, sig) && sig == kEocd && buf.u16(i + 20, comment) && i + 22 + comment <= buf.b.size()) {
          at = i;
          return true;
        }
        if (i == begin) return false;
        --i;
      }
    }

    struct Item {
      std::string name;
      bool directory = false;
      std::uint16_t method = 0;
      std::uint16_t flags = 0;
      std::uint32_t crc = 0;
      std::uint64_t comp = 0;
      std::uint64_t uncomp = 0;
      std::uint64_t local = 0;
    };

    bool
    safe_name(std::string raw, std::string &out, std::string &error) {
      for (char &ch : raw)
        if (ch == kSlash) ch = '/';
      if (raw.empty() || raw.find(char(0)) != std::string::npos) {
        error = "An entry name is empty";
        return false;
      }
      if (raw[0] == '/') {
        error = "An entry name is absolute";
        return false;
      }
      if (raw.size() >= 2 && ((raw[0] >= 'A' && raw[0] <= 'Z') || (raw[0] >= 'a' && raw[0] <= 'z')) && raw[1] == ':') {
        error = "An entry name has a drive letter";
        return false;
      }
      std::string built;
      std::size_t i = 0;
      const bool dir = !raw.empty() && raw.back() == '/';
      while (i < raw.size()) {
        const auto slash = raw.find('/', i);
        const auto part = raw.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (part.empty()) {
          if (slash == std::string::npos) break;
          i = slash + 1;
          continue;
        }
        if (part == "." || part == "..") {
          error = "An entry name leaves the install directory";
          return false;
        }
        if (part.find(':') != std::string::npos || part.find('*') != std::string::npos || part.find('?') != std::string::npos || part.find('"') != std::string::npos || part.find('<') != std::string::npos || part.find('>') != std::string::npos || part.find('|') != std::string::npos) {
          error = "An entry name has a character this system reserves";
          return false;
        }
        if (!built.empty()) built.push_back('/');
        built += part;
        if (slash == std::string::npos) break;
        i = slash + 1;
      }
      if (built.empty()) {
        error = "An entry name is empty";
        return false;
      }
      if (dir) built.push_back('/');
      out = std::move(built);
      return true;
    }

    bool
    apply_zip64(const Buf &buf, std::size_t extra, std::uint16_t extra_len, Item &item) {
      if (item.comp != 0xffffffffu && item.uncomp != 0xffffffffu && item.local != 0xffffffffu) return true;
      std::size_t p = extra;
      const std::size_t end = extra + extra_len;
      while (p + 4 <= end && p + 4 <= buf.b.size()) {
        std::uint16_t id = 0, sz = 0;
        if (!buf.u16(p, id) || !buf.u16(p + 2, sz) || p + 4 + sz > end || p + 4 + sz > buf.b.size()) return false;
        std::size_t q = p + 4;
        if (id == 0x0001) {
          if (item.uncomp == 0xffffffffu) {
            if (!buf.u64(q, item.uncomp)) return false;
            q += 8;
          }
          if (item.comp == 0xffffffffu) {
            if (!buf.u64(q, item.comp)) return false;
            q += 8;
          }
          if (item.local == 0xffffffffu) {
            if (!buf.u64(q, item.local)) return false;
          }
        }
        p += static_cast<std::size_t>(4 + sz);
      }
      return item.comp != 0xffffffffu && item.uncomp != 0xffffffffu && item.local != 0xffffffffu;
    }

    Outcome
    read_items(const Buf &buf, std::vector<Item> &items) {
      std::size_t eocd = 0;
      if (!find_eocd(buf, eocd)) return fail("bad_zip", "The package has no central directory");
      std::uint16_t disk = 0, entries = 0;
      std::uint32_t cd_size = 0, cd_at = 0;
      if (!buf.u16(eocd + 4, disk) || !buf.u16(eocd + 10, entries) || !buf.u32(eocd + 12, cd_size) || !buf.u32(eocd + 16, cd_at)) return fail("bad_zip", "The central directory is truncated");
      if (disk != 0 || entries == 0xffffu || cd_size == 0xffffffffu || cd_at == 0xffffffffu) return fail("bad_zip", "Spanned or ZIP64 end records are not installed");
      if (static_cast<std::uint64_t>(cd_at) + cd_size > buf.b.size()) return fail("bad_zip", "The central directory is outside the package");
      std::size_t at = cd_at;
      const std::size_t cd_end = static_cast<std::size_t>(cd_at) + cd_size;
      std::uint64_t total = 0;
      for (unsigned n = 0; n < entries; ++n) {
        if (items.size() >= kMaxEntries) return fail("bad_zip", "The package has too many entries");
        std::uint32_t sig = 0;
        if (!buf.u32(at, sig) || sig != kCentral) return fail("bad_zip", "A central directory entry is not valid");
        Item item;
        std::uint16_t name_len = 0, extra_len = 0, comment_len = 0;
        std::uint32_t comp32 = 0, uncomp32 = 0, local32 = 0, crc = 0;
        if (!buf.u16(at + 8, item.flags) || !buf.u16(at + 10, item.method) || !buf.u32(at + 16, crc) || !buf.u32(at + 20, comp32) || !buf.u32(at + 24, uncomp32) || !buf.u16(at + 28, name_len) || !buf.u16(at + 30, extra_len) || !buf.u16(at + 32, comment_len) || !buf.u32(at + 42, local32)) return fail("bad_zip", "A central directory entry is truncated");
        item.crc = crc;
        item.comp = comp32;
        item.uncomp = uncomp32;
        item.local = local32;
        const std::size_t name_at = at + 46;
        if (name_at + name_len + extra_len + comment_len > cd_end) return fail("bad_zip", "A central directory name is truncated");
        std::string raw(reinterpret_cast<const char *>(buf.b.data() + name_at), name_len);
        if (!apply_zip64(buf, name_at + name_len, extra_len, item)) return fail("bad_zip", "A ZIP64 size is truncated");
        if (item.flags & 1u) return fail("bad_zip", "An encrypted entry is not installed");
        if (item.method != 0 && item.method != 8) return fail("bad_zip", "Only stored and deflated entries are installed");
        if (item.uncomp > kMaxEntry || item.comp > kMaxEntry) return fail("bad_zip", "An entry is too large");
        total += item.uncomp;
        if (total > kMaxTotal) return fail("bad_zip", "The package expands too far");
        std::string name, why;
        if (!safe_name(raw, name, why)) return fail("zip_slip", why);
        item.directory = !name.empty() && name.back() == '/';
        item.name = std::move(name);
        items.push_back(std::move(item));
        at = name_at + name_len + extra_len + comment_len;
      }
      Outcome ok;
      ok.ok = true;
      ok.uncompressed = total;
      ok.entries = static_cast<unsigned>(items.size());
      return ok;
    }

    bool
    inside(const fs::path &root, const fs::path &file) {
      std::error_code ec;
      const fs::path base = fs::weakly_canonical(root, ec);
      if (ec) return false;
      fs::path probe = file;
      if (!fs::exists(probe, ec)) probe = probe.parent_path();
      const fs::path canon = fs::weakly_canonical(probe, ec);
      if (ec) return false;
      const auto rel = fs::relative(canon, base, ec);
      if (ec) return false;
      const std::string text = rel.generic_string();
      return text != ".." && text.rfind("../", 0) != 0;
    }

    bool
    write_bytes(const fs::path &root, const fs::path &file, const std::uint8_t *data, std::size_t size, std::string &error) {
      std::error_code ec;
      const fs::path rel = fs::relative(file, root, ec);
      if (ec) {
        error = "The entry path is not inside the install directory";
        return false;
      }
      fs::path cur = root;
      for (const auto &part : rel) {
        if (part == "." || part == "..") {
          error = "An entry name leaves the install directory";
          return false;
        }
        cur /= part;
        if (fs::is_symlink(cur, ec)) {
          error = "An entry would follow a link";
          return false;
        }
      }
      fs::create_directories(file.parent_path(), ec);
      if (ec) {
        error = "A directory could not be created";
        return false;
      }
      if (!inside(root, file)) {
        error = "An entry path is outside the install directory";
        return false;
      }
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      if (!out || (size && !out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size)))) {
        error = "A file could not be written";
        return false;
      }
      return true;
    }
  }  // namespace

  Outcome
  inspect(const fs::path &archive) {
    Buf buf;
    Outcome err;
    if (!load_file(archive, buf, err)) return err;
    std::vector<Item> items;
    return read_items(buf, items);
  }

  Outcome
  extract(const fs::path &archive, const fs::path &dest, const std::function<void(int)> &on_percent) {
    Buf buf;
    Outcome err;
    if (!load_file(archive, buf, err)) return err;
    std::vector<Item> items;
    Outcome meta = read_items(buf, items);
    if (!meta.ok) return meta;
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) return fail("not_writable", "The install directory could not be created");
    std::uint64_t wrote = 0;
    for (const Item &item : items) {
      const fs::path file = dest / fs::path(item.name);
      if (item.directory) {
        fs::create_directories(file, ec);
        continue;
      }
      if (item.local + 30 > buf.b.size()) return fail("bad_zip", "A local header is outside the package");
      std::uint32_t sig = 0;
      std::uint16_t name_len = 0, extra_len = 0;
      const std::size_t local = static_cast<std::size_t>(item.local);
      if (!buf.u32(local, sig) || sig != kLocal || !buf.u16(local + 26, name_len) || !buf.u16(local + 28, extra_len)) return fail("bad_zip", "A local header is truncated");
      const std::uint64_t data_at = item.local + 30ull + name_len + extra_len;
      if (data_at + item.comp > buf.b.size()) return fail("bad_zip", "Entry bytes are outside the package");
      const std::uint8_t *src = buf.b.data() + static_cast<std::size_t>(data_at);
      std::vector<std::uint8_t> plain;
      if (item.method == 0) {
        if (item.comp != item.uncomp) return fail("bad_zip", "A stored entry has the wrong size");
        plain.assign(src, src + static_cast<std::size_t>(item.comp));
      }
      else {
        plain.resize(static_cast<std::size_t>(item.uncomp == 0 ? 1 : item.uncomp));
        unsigned long dlen = static_cast<unsigned long>(item.uncomp);
        unsigned long slen = static_cast<unsigned long>(item.comp);
        const int rc = item.uncomp == 0 ? 0 : puff(plain.data(), &dlen, src, &slen);
        if (rc != 0 || dlen != item.uncomp) return fail("bad_zip", "A deflated entry could not be expanded");
        plain.resize(static_cast<std::size_t>(item.uncomp));
      }
      if (crc32_of(plain.data(), plain.size()) != item.crc) return fail("hash_mismatch", "An entry checksum does not match");
      std::string why;
      if (!write_bytes(dest, file, plain.data(), plain.size(), why)) {
        const std::string code = why.find("written") != std::string::npos ? "not_writable" : "zip_slip";
        return fail(code, why);
      }
      wrote += item.uncomp;
      if (on_percent && meta.uncompressed) on_percent(static_cast<int>(wrote * 100 / meta.uncompressed));
    }
    return meta;
  }
}  // namespace sparxie::zip
