// Node side of remote SD card downloads: answers the master's MSG_FILE_REQ from any
// file store (the CYD's SD card today, other boards later). Each request gets one
// MSG_FILE for a list page or an error, or up to req.count data chunks for a read.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ss_proto.h"

namespace ss {

class FileSource {
 public:
  virtual ~FileSource() = default;
  virtual bool ready() = 0;  // card in and readable
  // Calls fn once per file (name, size bytes), in a stable order.
  virtual void forEach(void (*fn)(const char* name, uint32_t size, void* ctx), void* ctx) = 0;
  // Reads up to len bytes at off. Returns bytes read (0 at the end), or -1 if the file
  // isn't there. *size = the file's size now.
  virtual int32_t readAt(const char* name, uint32_t off, uint8_t* buf, size_t len, uint32_t* size) = 0;
};

// send(payload, len, ctx) puts one MSG_FILE payload on the radio.
using FileSend = void (*)(const uint8_t* payload, size_t len, void* ctx);
// Returns the number of MSG_FILE payloads sent.
int serveFileReq(const FileReq& q, FileSource& src, FileSend send, void* ctx);

}  // namespace ss
