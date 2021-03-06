#pragma once

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <istream>
#include <ostream>
#include <fstream>

namespace torch { namespace jit {

// This file defines an on-disk serialization format to be used for PyTorch
// model serialization. All integer values are serialized as little-endian.
// Everything in this format is aligned to 64-byte boundaries to allow for direct
// memory mapping and use in, for example, AVX512 instructions.
// The format is as follows:
//
// -- File header --
// [8 bytes] Magic number - little endian integer that spells 'PYTORCH1' in ASCII
// [8 bytes] Version number - The version of this file format that this file is in.
//                            this allows us to revise and extend this format
// [56 bytes] Padding/reserved
//
// After the file header reside N records of the format
// [8 bytes] Tag - this is a tag that identifies the type of this record. The
//                 values are defined in the RecordTags enum below.
// [8 bytes] size - Size in bytes of the payload of this record
// [56 bytes] Pad/reserved - This space pads out the payload to a 64-byte alignment.
// [size bytes] Payload - The actual raw data for the object serialized in this record
// [size - (size % 64) bytes] Pad/reserved - pad out this record so the next
//                                                one is aligned to 64 bytes
//
// Following those records is a special footer:
// [8 bytes] Tag - This tag field should contain the value for RecordTags::FOOTER
//                 to correctly identify the footer
// [8 bytes] Offset of last record - The last record in this format is used
//                                   as an index into the rest of the file, so
//                                   a reader can use this offset to seek to
//                                   the last record and read the index.
// [56 bytes] Pad/reserved - Pad out the footer s.t. the whole file's size is a
//                           multiple of 64 bytes.
//
//
// When developing this format we want to pay particular attention to the
// following use cases:
//
// -- Reading --
// 1) Reading with full random access
//   a) Reading with file api's such as fread()
//   b) mmaping the file and jumping around the mapped region
// 2) Reading with 1-pass sequential access
//      -> A reader will need to build up a data structure of parsed structures
//         as it reads
//
// -- Writing --
// 1) Writing with full random access
// 2) Writing with 1-pass sequential access
//      -> We must take care not to require updating values that have already
//         been written. We place the variable-length index at the end and do
//         not put any indicies into the header to fulfill this constraint.

namespace {
  struct RecordTags {
    enum {
      STORAGE = 1,
      FOOTER = 2,
    };
  };

  // Common constants
  static constexpr uint64_t kFileMagicNumber = 0x314843524f545950L; // PYTORCH1
  static constexpr uint64_t kFieldAlignment = 64L; // 64 byte alignment supports up to AVX512 for mmap

  // Reader-specific constants
  static constexpr uint64_t kMaxSupportedFileFormatVersion = 0x1L;

  // Writer-specific constants
  static constexpr uint64_t kFileFormatVersion = 0x1L;
  static constexpr uint8_t kPadValue = 0xEF;

}  // namespace

class PyTorchStreamReader {
 public:
  PyTorchStreamReader(std::istream& in_) : in(in_) {
    // Store file size so we know when we're done reading because the f* APIs
    // don't do a good job of that
    in.seekg(0L, in.end);
    file_size = in.tellg();
    in.seekg(0L);
    readAndValidateFileHeader();
    // Do this now since we're reasonably sure this is actually a PyT file from
    // the header.
    if (file_size % kFieldAlignment != 0) {
      throw std::runtime_error("File length is not a multiple of the alignment"
                               " size. Is this a valid PyTorch file?");
    }
    readAndValidateFileFooter();
  }
  std::tuple<at::DataPtr, size_t> getLastRecord() {
    return getRecordWithKey(last_record_offset);
  }
  std::tuple<at::DataPtr, size_t> getRecordWithKey(uint64_t key) {
    if (key + kFieldAlignment > file_size) {
      throw std::runtime_error("Provided key is larger than the size of the file.");
    }
    if (key % kFieldAlignment != 0) {
      throw std::runtime_error("Provided key is not divisible by the alignment size.");
    }
    // Seek to the provided offset
    cursor = key;
    in.seekg(cursor);
    auto tag = read64BitIntegerLittleEndian();
    if (tag != RecordTags::STORAGE) {
      throw std::runtime_error("Attempted to read a record of non-storage type");
    }
    auto size = read64BitIntegerLittleEndian();
    seekToNextAlignmentBoundary();
    auto ptr = malloc(size);
    at::DataPtr retval(ptr, ptr, free, at::kCPU);

    in.read((char*)ptr, size);
    cursor += size;
    seekToNextAlignmentBoundary();
    return std::tuple<at::DataPtr, size_t>(std::move(retval), size);
  }
  ~PyTorchStreamReader() {
  }
 private:
  std::istream& in;
  size_t cursor = 0;
  size_t file_size;
  size_t last_record_offset;

  // Utility functions
  uint64_t read64BitIntegerLittleEndian() {
   uint64_t retval;
   // TODO endian swap on platforms that need it?
   in.read(reinterpret_cast<char *>(&retval), 8);
   std::streamsize read_bytes = in.gcount();
   if (read_bytes != 8) {
     std::ostringstream errmsg;
     errmsg << "Expected to read 8 bytes but got " << read_bytes;
     throw std::runtime_error(errmsg.str());
   }
   cursor += read_bytes;
   return retval;
  }

  void seekToNextAlignmentBoundary() {
   size_t next_offset = (cursor + kFieldAlignment) - (cursor % kFieldAlignment);
   size_t pad_amount = next_offset - cursor;
   cursor += pad_amount;
   in.seekg(cursor);
  }

  // File format deserialization functions
  void readAndValidateFileHeader() {
   // Validate magic number
   uint64_t magic = read64BitIntegerLittleEndian();
   if (magic != kFileMagicNumber) {
     throw std::runtime_error("Magic number mismatch in PyTorch file. File may"
                              " be corrupted or is not actually a PyTorch file.");
   }
   uint64_t file_format_version = read64BitIntegerLittleEndian();
   if (file_format_version > kMaxSupportedFileFormatVersion) {
     std::ostringstream errmsg;
     errmsg << "Attempted to read a PyTorch file with version " << file_format_version
            << " but the maximum supported version for reading is " << kMaxSupportedFileFormatVersion
            << ". Your PyTorch installation may be too old.";
     throw std::runtime_error(errmsg.str());
   }
   seekToNextAlignmentBoundary();
  }
  void readAndValidateFileFooter() {
    // Seek to location of file footer. We've already validated that the file
    // length is a multiple of the alignment size
    cursor = file_size - kFieldAlignment;
    in.seekg(cursor);
    auto tag = read64BitIntegerLittleEndian();
    if (tag != RecordTags::FOOTER) {
      throw std::runtime_error("File footer has wrong record type. Is this"
                               " file corrupted?");
    }
    last_record_offset = read64BitIntegerLittleEndian();
    if (last_record_offset > file_size) {
      throw std::runtime_error("Offset of last record is higher than the size"
                               " of the file! Is this file corrupted?");
    }
  }
};

class PyTorchStreamWriter {
 public:
  PyTorchStreamWriter(std::ostream& out_) : out(out_) {
    writeFileHeader();
    // In the case that we do not write any records into this file, the last
    // record index written into the footer will point to the footer itself.
    last_record_idx = cursor;
  }
  uint64_t writeRecord(const char* data, size_t size) {
    JIT_ASSERT(!finalized);
    uint64_t record_offset = cursor;
    last_record_idx = record_offset;
    write64BitIntegerLittleEndian(RecordTags::STORAGE);
    write64BitIntegerLittleEndian(size);
    padToNextAlignmentBoundary();
    writeBuffer(data, size);
    padToNextAlignmentBoundary();
    return record_offset;
  }
  void writeEndOfFile() {
    JIT_ASSERT(!finalized);
    writeFileFooter();
    finalized = true;
  }
  ~PyTorchStreamWriter() {
    if (!finalized) {
      writeEndOfFile();
    }
  }
 private:
  std::ostream& out;
  size_t cursor = 0;
  bool finalized = false;
  size_t last_record_idx = 0;

  // Utility functions
  void write64BitIntegerLittleEndian(const uint64_t value) {
    // TODO endian swap on platforms that need it?
    out.write(reinterpret_cast<const char *>(&value), 8);
    cursor += 8u;
  }

  void writePad(const size_t num_bytes) {
    static std::vector<char> pad_buffer(/*count=*/kFieldAlignment,
                                        /*value=*/kPadValue);
    out.write(pad_buffer.data(), num_bytes);
    cursor += num_bytes;
  }

  void padToNextAlignmentBoundary() {
    size_t next_offset = (cursor + kFieldAlignment) - (cursor % kFieldAlignment);
    size_t pad_amount = next_offset - cursor;
    writePad(pad_amount);
  }

  void writeBuffer(const char* data, size_t size) {
    out.write(data, size);
    cursor += size;
  }

  // File format write functions
  void writeFileHeader() {
    write64BitIntegerLittleEndian(kFileMagicNumber);
    write64BitIntegerLittleEndian(kFileFormatVersion);
    padToNextAlignmentBoundary();
  }

  void writeFileFooter() {
    write64BitIntegerLittleEndian(RecordTags::FOOTER);
    write64BitIntegerLittleEndian(last_record_idx);
    padToNextAlignmentBoundary();
  }
};

class PyTorchFileReader {
 public:
  PyTorchFileReader(const std::string& filename) :
    in(filename, std::ios_base::binary),
    stream_reader(in) {}

  std::tuple<at::DataPtr, size_t> getLastRecord() {
    return stream_reader.getLastRecord();
  }

  std::tuple<at::DataPtr, size_t> getRecordWithKey(uint64_t key) {
    return stream_reader.getRecordWithKey(key);
  }

 private:
  std::ifstream in;
  PyTorchStreamReader stream_reader;
};

class PyTorchFileWriter {
 public:
  PyTorchFileWriter(const std::string& filename) :
    out(filename, std::ios_base::binary),
    stream_writer(out) {}

  uint64_t writeRecord(const char* data, size_t size) {
    return stream_writer.writeRecord(data, size);
  }

  void writeEndOfFile() {
    stream_writer.writeEndOfFile();
    out.close();
  }

 private:
  std::ofstream out;
  PyTorchStreamWriter stream_writer;
};

}}  // namespace torch::jit
