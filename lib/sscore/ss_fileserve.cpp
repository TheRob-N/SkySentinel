#include "ss_fileserve.h"

#include <string.h>

namespace ss {

static int sendChunk(const FileChunk& c, FileSend send, void* ctx) {
  uint8_t buf[kMaxPayload];
  const size_t n = encodeFileChunk(c, buf, sizeof(buf));
  if (!n) return 0;
  send(buf, n, ctx);
  return 1;
}

static int sendError(uint8_t req, uint8_t err, FileSend send, void* ctx) {
  FileChunk c;
  c.req = req; c.kind = FK_ERROR; c.off = err;
  return sendChunk(c, send, ctx);
}

namespace {
struct ListPage {
  FileChunk c;
  uint32_t index = 0;   // entries seen so far
  bool full = false;
};
}  // namespace

static void addEntry(const char* name, uint32_t size, void* ctx) {
  ListPage& p = *(ListPage*)ctx;
  const uint32_t i = p.index++;
  if (i < p.c.off || p.full) return;
  size_t l = strlen(name);
  if (l > kMaxFileName) return;  // can't be asked for by name anyway
  if (p.c.len + 1 + l + 4 > kFileChunk) { p.full = true; return; }
  uint8_t* d = p.c.data + p.c.len;
  d[0] = (uint8_t)l;
  memcpy(d + 1, name, l);
  d[1 + l] = (uint8_t)(size >> 24); d[2 + l] = (uint8_t)(size >> 16);
  d[3 + l] = (uint8_t)(size >> 8);  d[4 + l] = (uint8_t)size;
  p.c.len = (uint8_t)(p.c.len + 1 + l + 4);
}

int serveFileReq(const FileReq& q, FileSource& src, FileSend send, void* ctx) {
  if (!src.ready()) return sendError(q.req, FE_NO_CARD, send, ctx);
  if (q.op == FO_LIST) {
    ListPage p;
    p.c.req = q.req; p.c.kind = FK_LIST; p.c.off = q.off;
    src.forEach(addEntry, &p);
    p.c.total = p.index;
    return sendChunk(p.c, send, ctx);
  }
  if (q.op != FO_READ) return 0;
  if (!fileNameOk(q.name)) return sendError(q.req, FE_BAD_NAME, send, ctx);
  const uint8_t count = q.count == 0 ? 1 : q.count > kFileWindow ? kFileWindow : q.count;
  int sent = 0;
  uint32_t off = q.off;
  for (uint8_t i = 0; i < count; ++i) {
    FileChunk c;
    c.req = q.req; c.kind = FK_DATA; c.off = off;
    const int32_t got = src.readAt(q.name, off, c.data, kFileChunk, &c.total);
    if (got < 0) return sent + sendError(q.req, i ? FE_READ : FE_NOT_FOUND, send, ctx);
    c.len = (uint8_t)got;
    sent += sendChunk(c, send, ctx);
    off += (uint32_t)got;
    if (got < (int32_t)kFileChunk || off >= c.total) break;  // that was the end
  }
  return sent;
}

}  // namespace ss
