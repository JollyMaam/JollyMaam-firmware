#include "package.h"
#include <string.h>
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int range(size_t size, size_t at, size_t n) { return at <= size && n <= size - at; }
static uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffff;
  while (size--) {
    crc ^= *data++;
    for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  return crc ^ 0xffffffff;
}
static int safe_name(const uint8_t *p, size_t n) {
  if (!n || p[0] == '/' || p[n - 1] == '/') return 0;
  size_t start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && (p[i] < 32 || p[i] == '\\' || p[i] == ':')) return 0;
    if (i == n || p[i] == '/') {
      size_t length = i - start;
      if (!length || (length == 1 && p[start] == '.') ||
          (length == 2 && p[start] == '.' && p[start + 1] == '.')) return 0;
      start = i + 1;
    }
  }
  return 1;
}
static int entry_at(const struct jm_package *p, size_t at, struct jm_package_entry *out, size_t *next) {
  if (!range(p->size, at, 46) || rd32(p->data + at) != 0x02014b50) return 0;
  const uint8_t *h = p->data + at;
  size_t name = rd16(h + 28), extra = rd16(h + 30), comment = rd16(h + 32);
  if (!range(p->size, at + 46, name + extra + comment)) return 0;
  size_t local = rd32(h + 42);
  if (!range(p->directory, local, 30) || rd32(p->data + local) != 0x04034b50) return 0;
  const uint8_t *l = p->data + local;
  if (rd16(l + 26) != name || rd16(l + 6) != rd16(h + 8) || rd16(l + 8) != rd16(h + 10) ||
      rd32(l + 14) != rd32(h + 16) || rd32(l + 18) != rd32(h + 20) ||
      rd32(l + 22) != rd32(h + 24)) return 0;
  size_t offset = local + 30, skip = name + rd16(l + 28);
  if (!range(p->directory, offset, skip) || memcmp(l + 30, h + 46, name)) return 0;
  offset += skip;
  size_t size = rd32(h + 24);
  if (!range(p->directory, offset, size)) return 0;
  out->name = h + 46; out->name_size = name; out->data = p->data + offset;
  out->size = size; out->crc = rd32(h + 16);
  *next = at + 46 + name + extra + comment;
  return 1;
}
/* Open a bounded ZIP32 stored-entry container into a borrowed view. No memory
 * allocation, decompression or filesystem I/O. Validate all entries and CRCs,
 * names, local/central consistency and duplicate names before returning.
 * Encrypted, compressed, multi-disk, ZIP64 and descriptor-based archives are
 * unsupported. At most 128 entries and 32 MiB are accepted. Output is untouched
 * on failure. Input storage must remain alive and unchanged while using views.
 * This validates the container, not manifest policy, SHA hashes or authenticity. */
enum jm_package_result jm_package_open(struct jm_package *out, const uint8_t *data, size_t size) {
  if (!out || !data || size < 22 || size > 32 * 1024 * 1024) return JM_PACKAGE_INVALID;
  size_t end = size - 22;
  for (;;) {
    if (rd32(data + end) == 0x06054b50 && rd16(data + end + 20) == size - end - 22) break;
    if (!end || size - end > 65557) return JM_PACKAGE_INVALID;
    --end;
  }
  const uint8_t *e = data + end;
  if (rd16(e + 4) || rd16(e + 6) || rd16(e + 8) != rd16(e + 10)) return JM_PACKAGE_UNSUPPORTED;
  struct jm_package p = {data, size, rd32(e + 16), rd16(e + 10)};
  if (!p.count || p.count > 128 || !range(end, p.directory, rd32(e + 12)) ||
      p.directory + rd32(e + 12) != end) return JM_PACKAGE_INVALID;
  size_t at = p.directory;
  for (unsigned i = 0; i < p.count; ++i) {
    if (!range(end, at, 46)) return JM_PACKAGE_INVALID;
    const uint8_t *h = data + at;
    if (rd16(h + 8) & ~0x800u || rd16(h + 10) || rd16(h + 34) ||
        rd32(h + 20) != rd32(h + 24)) return JM_PACKAGE_UNSUPPORTED;
    struct jm_package_entry file;
    size_t next;
    if (!entry_at(&p, at, &file, &next) || next > end || !safe_name(file.name, file.name_size)) return JM_PACKAGE_INVALID;
    if (crc32(file.data, file.size) != file.crc) return JM_PACKAGE_CRC;
    size_t prior = p.directory;
    for (unsigned j = 0; j < i; ++j) {
      struct jm_package_entry other; size_t following;
      if (!entry_at(&p, prior, &other, &following)) return JM_PACKAGE_INVALID;
      if (other.name_size == file.name_size && !memcmp(other.name, file.name, file.name_size)) return JM_PACKAGE_DUPLICATE;
      prior = following;
    }
    at = next;
  }
  if (at != end) return JM_PACKAGE_INVALID;
  *out = p; return JM_PACKAGE_OK;
}
/* Retrieve an entry from a successfully opened package. Name is not terminated;
 * data and name borrow the original buffer. Output is untouched on failure. */
int jm_package_entry(const struct jm_package *p, unsigned index, struct jm_package_entry *out) {
  if (!p || !out || index >= p->count) return 0;
  size_t at = p->directory;
  struct jm_package_entry file;
  for (unsigned i = 0; i <= index; ++i) {
    size_t next;
    if (!entry_at(p, at, &file, &next)) return 0;
    at = next;
  }
  *out = file; return 1;
}
/* Find an exact, case-sensitive archive name. Output is untouched if absent. */
int jm_package_find(const struct jm_package *p, const char *name, struct jm_package_entry *out) {
  if (!p || !name || !out) return 0;
  size_t length = strlen(name);
  for (unsigned i = 0; i < p->count; ++i) {
    struct jm_package_entry file;
    if (!jm_package_entry(p, i, &file)) return 0;
    if (file.name_size == length && !memcmp(file.name, name, length)) { *out = file; return 1; }
  }
  return 0;
}
