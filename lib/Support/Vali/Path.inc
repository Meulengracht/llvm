//===- llvm/Support/Windows/Path.inc - Windows Path Impl --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Windows specific implementation of the Path API.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include <os/mollenos.h>
#include <errno.h>
#include <io.h>

using namespace llvm;

static bool is_separator(const wchar_t value) {
  switch (value) {
  case L'\\':
  case L'/':
    return true;
  default:
    return false;
  }
}

namespace llvm {
namespace sys  {
namespace fs {

// Implemented
std::string getMainExecutable(const char *argv0, void *MainExecAddr) {
  return std::string(argv0.data());
}

// Implemented
UniqueID file_status::getUniqueID() const {
  return UniqueID((uint64_t)VolumeSerialNumber, (uint64_t)FileSerialNumber);
}

// Implemented
ErrorOr<space_info> disk_space(const Twine &Path) {
  vStorageDescriptor_t Descriptor;
  if (!::GetStorageInformationFromPath(Path.str().c_str(), &Descriptor))
    return make_error_code(no_such_file_or_directory);
  space_info SpaceInfo;
  SpaceInfo.capacity = (static_cast<uint64_t>(Descriptor.BytesTotal.u.HighPart) << 32) + Descriptor.BytesTotal.u.LowPart;
  SpaceInfo.free = (static_cast<uint64_t>(Descriptor.BytesFree.u.HighPart) << 32) + Descriptor.BytesFree.u.LowPart;
  SpaceInfo.available = (static_cast<uint64_t>(Descriptor.BytesAvailable.u.HighPart) << 32) + Descriptor.BytesAvailable.u.LowPart;
  return SpaceInfo;
}

// Implemented
TimePoint<> basic_file_status::getLastAccessedTime() const {
  return toTimePoint(LastAccessed);
}

// Implemented
TimePoint<> basic_file_status::getLastModificationTime() const {
  return toTimePoint(LastModified);
}

// Implemented
uint32_t file_status::getLinkCount() const {
  return NumLinks;
}

// Implemented
std::error_code current_path(SmallVectorImpl<char> &result) {
  SmallVector<char, MAX_PATH> cur_path;
  if (GetWorkingDirectory(cur_path.data(), MAX_PATH) != OsSuccess) {
      return make_error_code(invalid_argument);
  }
  cur_path.set_size(strlen(cur_path.data()) + 1);
  result.clear();
  result.append(cur_path.begin(), cur_path.end());
  return std::error_code();
}

// Implemented
std::error_code set_current_path(const Twine &path) {
  if (SetWorkingDirectory(path.begin()) != OsSuccess) {
      return make_error_code(no_such_file_or_directory);
  }
  return std::error_code();
}

// Implemented
std::error_code create_directory(const Twine &path, bool IgnoreExisting,
                                 perms Perms) {
  int ec = ::_opendir(path.begin(), _O_CREAT | _O_RDWR);
  if (fd == -1) {
      if (errno == EACCES) {
          return make_error_code(permission_denied);
      }
      if (errno == EEXIST) {
          return IgnoreExisting ? std::error_code() : make_error_code(file_exists);
      }
      return std::error_code(errno, std::generic_category());
  }
  ::_closedir(fd);
  return std::error_code();
}

// Implemented
std::error_code create_link(const Twine &to, const Twine &from) {
  if (::_link(from.begin(), to.begin(), 1) != 0) {
      return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

// Implemented
std::error_code create_hard_link(const Twine &to, const Twine &from) {
  if (::_link(from.begin(), to.begin(), 0) != 0) {
      return std::error_code(errno, std::generic_category());
  }
  return std::error_code();
}

// Implemented
std::error_code remove(const Twine &path, bool IgnoreNonExisting) {
    if (!::remove(path.begin())) {
        return std::error_code();
    }
    if (IgnoreNonExisting && errno == ENOENT) {
        return std::error_code();
    }
    return std::error_code(errno, std::generic_category());
}

// Implemented
std::error_code is_local(const Twine &path, bool &result) {
  if (!llvm::sys::fs::exists(path) || !llvm::sys::path::has_root_path(path))
    return make_error_code(errc::no_such_file_or_directory);
  vStorageDescriptor_t Descriptor;
  if (GetStorageInformationFromPath(path.begin(), &Descriptor) != OsSuccess) {
    return make_error_code(errc::no_such_file_or_directory);
  }
  if (Descriptor.Flags & STORAGE_STATIC) {
    result = true;
    return std::error_code();
  }
  result = false;
  return std::error_code();
}

// Implemented
std::error_code is_local(int FD, bool &Result) {
  vStorageDescriptor_t Descriptor;
  if (GetStorageInformationFromFd(FD, &Descriptor) != OsSuccess) {
    return make_error_code(errc::no_such_file_or_directory);
  }
  if (Descriptor.Flags & STORAGE_STATIC) {
    result = true;
    return std::error_code();
  }
  result = false;
  return std::error_code();
}

// Implemented
std::error_code rename(const Twine &From, const Twine &To) {
  // Get arguments.
  SmallString<256> from_storage;
  SmallString<256> to_storage;
  StringRef f = From.toNullTerminatedStringRef(from_storage);
  StringRef t = To.toNullTerminatedStringRef(to_storage);
  if (::rename(f.begin(), t.begin()) == -1)
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

// Implemented
std::error_code resize_file(int FD, uint64_t Size) {
  ::_chsize(FD, Size);
  return std::error_code(error, std::generic_category());
}

// Implemented
std::error_code access(const Twine &Path, AccessMode Mode) {
    vFileDescriptor_t Descriptor;
    SmallString<256> path_storage;
    StringRef f = Path.toNullTerminatedStringRef(path_storage);
    if (GetFileInformationFromPath(f.begin(), &Descriptor) != OsSuccess) {
        return errc::no_such_file_or_directory;
    }
    if (Mode == AccessMode::Write && !(Descriptor.Permissions & FILE_PERMISSION_WRITE)) {
        return errc::permission_denied;
    }
    if (Mode == AccessMode::Execute && !(Descriptor.Permissions & FILE_PERMISSION_EXECUTE)) {
        return errc::permission_denied;
    }
    return std::error_code();
}

// Implemented
bool can_execute(const Twine &Path) {
  return !access(Path, AccessMode::Execute) ||
         !access(Path + ".app", AccessMode::Execute);
}

// Implemented
bool equivalent(file_status A, file_status B) {
  assert(status_known(A) && status_known(B));
  return A.FileSizeHigh          == B.FileSizeHigh &&
         A.FileSizeLow           == B.FileSizeLow &&
         A.LastAccessed          == B.LastAccessed &&
         A.LastModified          == B.LastModified &&
         A.VolumeSerialNumber    == B.VolumeSerialNumber &&
         A.FileSerialNumber      == B.FileSerialNumber;
}

// Implemented
std::error_code equivalent(const Twine &A, const Twine &B, bool &result) {
  file_status fsA, fsB;
  if (std::error_code ec = status(A, fsA))
    return ec;
  if (std::error_code ec = status(B, fsB))
    return ec;
  result = equivalent(fsA, fsB);
  return std::error_code();
}

// @todo
static bool isReservedName(StringRef path) {
  return false;
}

// Implemented
static file_type file_type_from_flags(Flags_t Flags) {
    return (Attrs & FILE_FLAG_DIRECTORY) ? file_type::directory_file : file_type::regular_file;
}

// Implemented
static perms perms_from_flags(Flags_t Permissions) {
    return (Attrs & FILE_ATTRIBUTE_READONLY) ? (all_read | all_exe) : all_all;
}

// Implemented
std::error_code status(const Twine &path, file_status &result, bool Follow) {
    vFileDescriptor_t Descriptor;
    SmallString<256> path_storage;
    StringRef f = path.toNullTerminatedStringRef(path_storage);
    if (GetFileInformationFromPath(f.begin(), &Descriptor) != OsSuccess) {
        return errc::no_such_file_or_directory;
    }
    result = file_status(
        file_type_from_flags(Descriptor.Flags), perms_from_flags(Descriptor.Permissions), 
        1, Descriptor.AccessedAt.tv_sec, Descriptor.ModifiedAt.tv_sec,
        Descriptor.StorageId, Descriptor.Id, Info.nFileSizeHigh, Info.nFileSizeLow);
    return std:error_code();
}

// Implemented
std::error_code status(int FD, file_status &Result) {
    vFileDescriptor_t Descriptor;
    if (GetFileInformationFromFd(FD, &Descriptor) != OsSuccess) {
        return errc::no_such_file_or_directory;
    }
    result = file_status(
        file_type_from_flags(Descriptor.Flags), perms_from_flags(Descriptor.Permissions), 
        1, Descriptor.AccessedAt.tv_sec, Descriptor.ModifiedAt.tv_sec,
        Descriptor.StorageId, Descriptor.Id, Info.nFileSizeHigh, Info.nFileSizeLow);
    return std:error_code();
}

// @todo
std::error_code setPermissions(const Twine &Path, perms Permissions) {
  return std::error_code();
}

// @todo
std::error_code setLastModificationAndAccessTime(int FD, TimePoint<> Time) {
  return std::error_code();
}

// Implemented
std::error_code mapped_file_region::init(int FD, uint64_t Offset, mapmode Mode) {
  int Flags = (Mode == readonly) ? FILE_MAPPING_READ : (FILE_MAPPING_READ | FILE_MAPPING_WRITE);
  assert(Size != 0);
  if (::CreateFileMapping(FD, Flags, Offset, Size, &Mapping) != OsSuccess)
    return std::error_code(errno, std::generic_category());
  return std::error_code();
}

// Implemented
mapped_file_region::mapped_file_region(int fd, mapmode mode, size_t length,
                                       uint64_t offset, std::error_code &ec)
    : Size(length), Mapping() {
  ec = init(fd, offset, mode);
  if (ec)
    Mapping = nullptr;
}

// Implemented
mapped_file_region::~mapped_file_region() {
  if (Mapping)
    ::DestroyFileMapping(Mapping);
}

// Implemented
size_t mapped_file_region::size() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return Size;
}

// Implemented
char *mapped_file_region::data() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return reinterpret_cast<char*>(Mapping);
}

// Implemented
const char *mapped_file_region::const_data() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return reinterpret_cast<const char*>(Mapping);
}

// Implemented
int mapped_file_region::alignment() {
  return Process::getPageSize();
}

// Implemented
std::error_code detail::directory_iterator_construct(detail::DirIterState &it,
                                                     StringRef path,
                                                     bool follow_symlinks) {
  SmallString<128> path_null(path);
  struct directory_handle *directory = nullptr; 
  int ec = ::_opendir(path_null.c_str(), _O_RDONLY, &directory);
  if (!directory || ec) {
      return std::error_code(ec, std::generic_category());
  }

  it.IterationHandle = reinterpret_cast<intptr_t>(directory);
  // Add something for replace_filename to replace.
  path::append(path_null, ".");
  it.CurrentEntry = directory_entry(path_null.str(), follow_symlinks);
  return directory_iterator_increment(it);
}

// Implemented
std::error_code detail::directory_iterator_destruct(detail::DirIterState &it) {
  if (it.IterationHandle)
    ::_closedir(reinterpret_cast<struct directory_handle*>(it.IterationHandle));
  it.IterationHandle = 0;
  it.CurrentEntry = directory_entry();
  return std::error_code();
}

// Implemented
std::error_code detail::directory_iterator_increment(detail::DirIterState &it) {
  struct directory_entry dentry;
  errno = 0;
  int ec = ::_readdir(reinterpret_cast<struct directory_handle*>(it.IterationHandle), &dentry);
  if (ec != EOK && ec != EOF) {
    return std::error_code(ec, std::generic_category());
  } else if (ec != EOF) {
    StringRef name(&dentry.d_name[0], strlen(&dentry.d_name[0]));
    if ((name.size() == 1 && name[0] == '.') ||
        (name.size() == 2 && name[0] == '.' && name[1] == '.'))
      return directory_iterator_increment(it);
    it.CurrentEntry.replace_filename(name);
  } else
    return directory_iterator_destruct(it);

  return std::error_code();
}

// Implemented
ErrorOr<basic_file_status> directory_entry::status() const {
  return Status;
}

// Implemented
std::error_code openFileForRead(const Twine &Name, int &ResultFD, SmallVectorImpl<char> *RealPath) {
    SmallString<256> name_storage;
    StringRef f = Name.toNullTerminatedStringRef(name_storage);
    int fd = _open(f.begin(), _O_RDONLY);
    if (fd == -1) {
        return std::error_code(errno, std::generic_category());
    }
    ResultFD = fd;
    if (RealPath) {
        RealPath->clear();
        RealPath->reserve(MAXPATH);
        if (GetFilePathFromFd(fd, (char*)this->begin(), MAXPATH) != OsSuccess) {
            return std::error_code(errno, std::generic_category());
        }
        RealPath->set_size(strlen(this->begin()) + 1);
    }
    return std:error_code();
}

// Implemented
std::error_code openFileForWrite(const Twine &Name, int &ResultFD,
                            sys::fs::OpenFlags Flags, unsigned Mode) {
  // Verify that we don't have both "append" and "excl".
  assert((!(Flags & sys::fs::F_Excl) || !(Flags & sys::fs::F_Append)) &&
         "Cannot specify both 'excl' and 'append' file creation flags!");
  SmallString<256> name_storage;
  StringRef f = Name.toNullTerminatedStringRef(name_storage);
  int oflags = _O_RDWR;

  // Resolve creation flags
  if (Flags & F_Excl) {
    oflags |= _O_EXCL;
  }
  else if (Flags & F_Append) {
    oflags |= _O_APPEND;      
  }
  else {
    oflags |= _O_CREAT;
  }
  if (Flags & F_Delete) {
    oflags |= _O_TEMPORARY;
  }
  if (Flags & F_Text) {
    oflags |= _O_TEXT;
  }

  // open
  int fd = _open(f.begin(), oflags);
  if (fd == -1) {
      return std::error_code(errno, std::generic_category());
  }
  ResultFD = fd;
  return std::error_code();
}

// Implemented
template <typename T>
static std::error_code remove_directories_impl(const T &Entry,
                                               bool IgnoreErrors) {
  std::error_code EC;
  directory_iterator Begin(Entry, EC, false);
  directory_iterator End;
  while (Begin != End) {
    auto &Item = *Begin;
    ErrorOr<basic_file_status> st = Item.status();
    if (!st && !IgnoreErrors)
      return st.getError();

    if (is_directory(*st)) {
      EC = remove_directories_impl(Item, IgnoreErrors);
      if (EC && !IgnoreErrors)
        return EC;
    }

    EC = fs::remove(Item.path(), true);
    if (EC && !IgnoreErrors)
      return EC;

    Begin.increment(EC);
    if (EC && !IgnoreErrors)
      return EC;
  }
  return std::error_code();
}

// Implemented
std::error_code remove_directories(const Twine &path, bool IgnoreErrors) {
  auto EC = remove_directories_impl(path, IgnoreErrors);
  if (EC && !IgnoreErrors)
    return EC;
  EC = fs::remove(path, true);
  if (EC && !IgnoreErrors)
    return EC;
  return std::error_code();
}

// Implemented
static void expandTildeExpr(SmallVectorImpl<char> &Path) {
  // Path does not begin with a tilde expression.
  if (Path.empty() || Path[0] != '~')
    return;

  StringRef PathStr(Path.begin(), Path.size());
  PathStr = PathStr.drop_front();
  StringRef Expr = PathStr.take_until([](char c) { return path::is_separator(c); });

  if (!Expr.empty()) {
    // This is probably a ~username/ expression. no support
    return;
  }

  SmallString<128> HomeDir;
  if (!path::home_directory(HomeDir)) {
    // For some reason we couldn't get the home directory.  Just exit.
    return;
  }

  // Overwrite the first character and insert the rest.
  Path[0] = HomeDir[0];
  Path.insert(Path.begin() + 1, HomeDir.begin() + 1, HomeDir.end());
}

// Implemented
std::error_code real_path(const Twine &path, SmallVectorImpl<char> &dest,
                          bool expand_tilde) {
  dest.clear();
  if (path.isTriviallyEmpty())
    return std::error_code();

  if (expand_tilde) {
    SmallString<128> Storage;
    path.toVector(Storage);
    expandTildeExpr(Storage);
    return real_path(Storage, dest, false);
  }

  if (is_directory(path))
    return directoryRealPath(path, dest);

  int fd;
  if (std::error_code EC = llvm::sys::fs::openFileForRead(path, fd, &dest))
    return EC;
  ::close(fd);
  return std::error_code();
}

} // end namespace fs


// Implemented
namespace path {
bool getUserCacheDir(SmallVectorImpl<char> &Result) {
    SmallVector<char, MAXPATH> Buf;
    if (GetUserCacheDirectory(Buf.data(), Buf.capacity()) != OsSuccess) {
        return false;
    }
    Buf.set_size(strlen(Buf.data()) + 1);
    result.append(Buf.data(), strlen(Buf.data()) + 1);
    return true;
}

bool home_directory(SmallVectorImpl<char> &result) {
    SmallVector<char, MAXPATH> Buf;
    if (GetUserDirectory(Buf.data(), Buf.capacity()) != OsSuccess) {
        return false;
    }
    Buf.set_size(strlen(Buf.data()) + 1);
    result.append(Buf.data(), strlen(Buf.data()) + 1);
    return true;
}

void system_temp_directory(bool ErasedOnReboot, SmallVectorImpl<char> &Result) {
    SmallVector<char, MAXPATH> Buf;
    Result.clear();
    if (ErasedOnReboot) {
        if (GetApplicationTemporaryDirectory(Buf.data(), Buf.capacity()) != OsSuccess) {
            return;
        }
    }
    else {
        if (GetApplicationDirectory(Buf.data(), Buf.capacity()) != OsSuccess) {
            return;
        }
    }
    Buf.set_size(strlen(Buf.data()) + 1);
    result.append(Buf.data(), strlen(Buf.data()) + 1);
}
} // end namespace path
} // end namespace sys
} // end namespace llvm
