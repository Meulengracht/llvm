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

std::string getMainExecutable(const char *argv0, void *MainExecAddr) {
  return std::string(argv0.data());
}

UniqueID file_status::getUniqueID() const {
  // The file is uniquely identified by the volume serial number along
  // with the 64-bit file identifier.
  uint64_t FileID = (static_cast<uint64_t>(FileIndexHigh) << 32ULL) |
                    static_cast<uint64_t>(FileIndexLow);
  return UniqueID(VolumeSerialNumber, FileID);
}

ErrorOr<space_info> disk_space(const Twine &Path) {
  ULARGE_INTEGER Avail, Total, Free;
  if (!::GetDiskFreeSpaceExA(Path.str().c_str(), &Avail, &Total, &Free))
    return mapWindowsError(::GetLastError());
  space_info SpaceInfo;
  SpaceInfo.capacity =
      (static_cast<uint64_t>(Total.HighPart) << 32) + Total.LowPart;
  SpaceInfo.free = (static_cast<uint64_t>(Free.HighPart) << 32) + Free.LowPart;
  SpaceInfo.available =
      (static_cast<uint64_t>(Avail.HighPart) << 32) + Avail.LowPart;
  return SpaceInfo;
}

TimePoint<> basic_file_status::getLastAccessedTime() const {
  FILETIME Time;
  Time.dwLowDateTime = LastAccessedTimeLow;
  Time.dwHighDateTime = LastAccessedTimeHigh;
  return toTimePoint(Time);
}

TimePoint<> basic_file_status::getLastModificationTime() const {
  FILETIME Time;
  Time.dwLowDateTime = LastWriteTimeLow;
  Time.dwHighDateTime = LastWriteTimeHigh;
  return toTimePoint(Time);
}

uint32_t file_status::getLinkCount() const {
  return NumLinks;
}

std::error_code current_path(SmallVectorImpl<char> &result) {
  SmallVector<char, MAX_PATH> cur_path;
  if (PathQueryWorkingDirectory(cur_path.data(), MAX_PATH) != OsSuccess) {
      return make_error_code(invalid_argument);
  }
  cur_path.set_size(strlen(cur_path.data()) + 1);
  result.clear();
  result.append(cur_path.begin(), cur_path.end());
  return std::error_code();
}

std::error_code set_current_path(const Twine &path) {
  if (PathChangeWorkingDirectory(path.begin()) != OsSuccess) {
      return make_error_code(no_such_file_or_directory);
  }
  return std::error_code();
}

std::error_code create_directory(const Twine &path, bool IgnoreExisting,
                                 perms Perms) {
  int fd = ::_opendir(path.begin(), _O_CREAT | _O_RDWR);
  if (fd == -1) {
      if (errno == EACCES) {
          return make_error_code(permission_denied);
      }
      if (errno == EEXIST) {
          return make_error_code(file_exists);
      }
      return make_error_code(no_such_file_or_directory);
  }
  ::_closedir(fd);
  return std::error_code();
}

std::error_code create_link(const Twine &to, const Twine &from) {
  if (::_link(from.begin(), to.begin(), 1) != 0) {
      return make_error_code(no_such_file_or_directory);
  }
  return std::error_code();
}

std::error_code create_hard_link(const Twine &to, const Twine &from) {
  if (::_link(from.begin(), to.begin(), 0) != 0) {
      return make_error_code(no_such_file_or_directory);
  }
  return std::error_code();
}

std::error_code remove(const Twine &path, bool IgnoreNonExisting) {
    if (!::remove(path.begin())) {
        return std::error_code();
    }
    return make_error_code(no_such_file_or_directory);
}

static std::error_code is_local_internal(SmallVectorImpl<wchar_t> &Path,
                                         bool &Result) {
  SmallVector<wchar_t, 128> VolumePath;
  size_t Len = 128;
  while (true) {
    VolumePath.resize(Len);
    BOOL Success =
        ::GetVolumePathNameW(Path.data(), VolumePath.data(), VolumePath.size());

    if (Success)
      break;

    DWORD Err = ::GetLastError();
    if (Err != ERROR_INSUFFICIENT_BUFFER)
      return mapWindowsError(Err);

    Len *= 2;
  }
  // If the output buffer has exactly enough space for the path name, but not
  // the null terminator, it will leave the output unterminated.  Push a null
  // terminator onto the end to ensure that this never happens.
  VolumePath.push_back(L'\0');
  VolumePath.set_size(wcslen(VolumePath.data()));
  const wchar_t *P = VolumePath.data();

  UINT Type = ::GetDriveTypeW(P);
  switch (Type) {
  case DRIVE_FIXED:
    Result = true;
    return std::error_code();
  case DRIVE_REMOTE:
  case DRIVE_CDROM:
  case DRIVE_RAMDISK:
  case DRIVE_REMOVABLE:
    Result = false;
    return std::error_code();
  default:
    return make_error_code(errc::no_such_file_or_directory);
  }
  llvm_unreachable("Unreachable!");
}

std::error_code is_local(const Twine &path, bool &result) {
  if (!llvm::sys::fs::exists(path) || !llvm::sys::path::has_root_path(path))
    return make_error_code(errc::no_such_file_or_directory);

  SmallString<128> Storage;
  StringRef P = path.toStringRef(Storage);

  // Convert to utf-16.
  SmallVector<wchar_t, 128> WidePath;
  if (std::error_code ec = widenPath(P, WidePath))
    return ec;
  return is_local_internal(WidePath, result);
}

static std::error_code realPathFromHandle(HANDLE H,
                                          SmallVectorImpl<wchar_t> &Buffer) {
  DWORD CountChars = ::GetFinalPathNameByHandleW(
      H, Buffer.begin(), Buffer.capacity() - 1, FILE_NAME_NORMALIZED);
  if (CountChars > Buffer.capacity()) {
    // The buffer wasn't big enough, try again.  In this case the return value
    // *does* indicate the size of the null terminator.
    Buffer.reserve(CountChars);
    CountChars = ::GetFinalPathNameByHandleW(
        H, Buffer.data(), Buffer.capacity() - 1, FILE_NAME_NORMALIZED);
  }
  if (CountChars == 0)
    return mapWindowsError(GetLastError());
  Buffer.set_size(CountChars);
  return std::error_code();
}

static std::error_code realPathFromHandle(HANDLE H,
                                          SmallVectorImpl<char> &RealPath) {
  RealPath.clear();
  SmallVector<wchar_t, MAX_PATH> Buffer;
  if (std::error_code EC = realPathFromHandle(H, Buffer))
    return EC;

  const wchar_t *Data = Buffer.data();
  DWORD CountChars = Buffer.size();
  if (CountChars >= 4) {
    if (0 == ::memcmp(Data, L"\\\\?\\", 8)) {
      CountChars -= 4;
      Data += 4;
    }
  }

  // Convert the result from UTF-16 to UTF-8.
  return UTF16ToUTF8(Data, CountChars, RealPath);
}

std::error_code is_local(int FD, bool &Result) {
  SmallVector<wchar_t, 128> FinalPath;
  HANDLE Handle = reinterpret_cast<HANDLE>(_get_osfhandle(FD));

  if (std::error_code EC = realPathFromHandle(Handle, FinalPath))
    return EC;

  return is_local_internal(FinalPath, Result);
}

std::error_code rename(const Twine &From, const Twine &To) {
  // Get arguments.
  SmallString<128> from_storage;
  SmallString<128> to_storage;
  StringRef f = from.toNullTerminatedStringRef(from_storage);
  StringRef t = to.toNullTerminatedStringRef(to_storage);

  if (::rename(f.begin(), t.begin()) == -1)
    return std::error_code(errno, std::generic_category());

  return std::error_code();
}

std::error_code resize_file(int FD, uint64_t Size) {
#ifdef HAVE__CHSIZE_S
  errno_t error = ::_chsize_s(FD, Size);
#else
  errno_t error = ::_chsize(FD, Size);
#endif
  return std::error_code(error, std::generic_category());
}

std::error_code access(const Twine &Path, AccessMode Mode) {
  SmallVector<wchar_t, 128> PathUtf16;

  if (std::error_code EC = widenPath(Path, PathUtf16))
    return EC;

  DWORD Attributes = ::GetFileAttributesW(PathUtf16.begin());

  if (Attributes == INVALID_FILE_ATTRIBUTES) {
    // See if the file didn't actually exist.
    DWORD LastError = ::GetLastError();
    if (LastError != ERROR_FILE_NOT_FOUND &&
        LastError != ERROR_PATH_NOT_FOUND)
      return mapWindowsError(LastError);
    return errc::no_such_file_or_directory;
  }

  if (Mode == AccessMode::Write && (Attributes & FILE_ATTRIBUTE_READONLY))
    return errc::permission_denied;

  return std::error_code();
}

bool can_execute(const Twine &Path) {
  return !access(Path, AccessMode::Execute) ||
         !access(Path + ".exe", AccessMode::Execute);
}

bool equivalent(file_status A, file_status B) {
  assert(status_known(A) && status_known(B));
  return A.FileIndexHigh         == B.FileIndexHigh &&
         A.FileIndexLow          == B.FileIndexLow &&
         A.FileSizeHigh          == B.FileSizeHigh &&
         A.FileSizeLow           == B.FileSizeLow &&
         A.LastAccessedTimeHigh  == B.LastAccessedTimeHigh &&
         A.LastAccessedTimeLow   == B.LastAccessedTimeLow &&
         A.LastWriteTimeHigh     == B.LastWriteTimeHigh &&
         A.LastWriteTimeLow      == B.LastWriteTimeLow &&
         A.VolumeSerialNumber    == B.VolumeSerialNumber;
}

std::error_code equivalent(const Twine &A, const Twine &B, bool &result) {
  file_status fsA, fsB;
  if (std::error_code ec = status(A, fsA))
    return ec;
  if (std::error_code ec = status(B, fsB))
    return ec;
  result = equivalent(fsA, fsB);
  return std::error_code();
}

static bool isReservedName(StringRef path) {
  return false;
}

static file_type file_type_from_attrs(DWORD Attrs) {
  return (Attrs & FILE_ATTRIBUTE_DIRECTORY) ? file_type::directory_file
                                            : file_type::regular_file;
}

static perms perms_from_attrs(DWORD Attrs) {
  return (Attrs & FILE_ATTRIBUTE_READONLY) ? (all_read | all_exe) : all_all;
}

static std::error_code getStatus(HANDLE FileHandle, file_status &Result) {
  if (FileHandle == INVALID_HANDLE_VALUE)
    goto handle_status_error;

  switch (::GetFileType(FileHandle)) {
  default:
    llvm_unreachable("Don't know anything about this file type");
  case FILE_TYPE_UNKNOWN: {
    DWORD Err = ::GetLastError();
    if (Err != NO_ERROR)
      return mapWindowsError(Err);
    Result = file_status(file_type::type_unknown);
    return std::error_code();
  }
  case FILE_TYPE_DISK:
    break;
  case FILE_TYPE_CHAR:
    Result = file_status(file_type::character_file);
    return std::error_code();
  case FILE_TYPE_PIPE:
    Result = file_status(file_type::fifo_file);
    return std::error_code();
  }

  BY_HANDLE_FILE_INFORMATION Info;
  if (!::GetFileInformationByHandle(FileHandle, &Info))
    goto handle_status_error;

  Result = file_status(
      file_type_from_attrs(Info.dwFileAttributes),
      perms_from_attrs(Info.dwFileAttributes), Info.nNumberOfLinks,
      Info.ftLastAccessTime.dwHighDateTime, Info.ftLastAccessTime.dwLowDateTime,
      Info.ftLastWriteTime.dwHighDateTime, Info.ftLastWriteTime.dwLowDateTime,
      Info.dwVolumeSerialNumber, Info.nFileSizeHigh, Info.nFileSizeLow,
      Info.nFileIndexHigh, Info.nFileIndexLow);
  return std::error_code();

handle_status_error:
  DWORD LastError = ::GetLastError();
  if (LastError == ERROR_FILE_NOT_FOUND ||
      LastError == ERROR_PATH_NOT_FOUND)
    Result = file_status(file_type::file_not_found);
  else if (LastError == ERROR_SHARING_VIOLATION)
    Result = file_status(file_type::type_unknown);
  else
    Result = file_status(file_type::status_error);
  return mapWindowsError(LastError);
}

std::error_code status(const Twine &path, file_status &result, bool Follow) {
  SmallString<128> path_storage;
  SmallVector<wchar_t, 128> path_utf16;

  StringRef path8 = path.toStringRef(path_storage);
  if (isReservedName(path8)) {
    result = file_status(file_type::character_file);
    return std::error_code();
  }

  if (std::error_code ec = widenPath(path8, path_utf16))
    return ec;

  DWORD attr = ::GetFileAttributesW(path_utf16.begin());
  if (attr == INVALID_FILE_ATTRIBUTES)
    return getStatus(INVALID_HANDLE_VALUE, result);

  DWORD Flags = FILE_FLAG_BACKUP_SEMANTICS;
  // Handle reparse points.
  if (!Follow && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
    Flags |= FILE_FLAG_OPEN_REPARSE_POINT;

  ScopedFileHandle h(
      ::CreateFileW(path_utf16.begin(), 0, // Attributes only.
                    FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, Flags, 0));
  if (!h)
    return getStatus(INVALID_HANDLE_VALUE, result);

  return getStatus(h, result);
}

std::error_code status(int FD, file_status &Result) {
  HANDLE FileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(FD));
  return getStatus(FileHandle, Result);
}

std::error_code setPermissions(const Twine &Path, perms Permissions) {
  SmallVector<wchar_t, 128> PathUTF16;
  if (std::error_code EC = widenPath(Path, PathUTF16))
    return EC;

  DWORD Attributes = ::GetFileAttributesW(PathUTF16.begin());
  if (Attributes == INVALID_FILE_ATTRIBUTES)
    return mapWindowsError(GetLastError());

  // There are many Windows file attributes that are not to do with the file
  // permissions (e.g. FILE_ATTRIBUTE_HIDDEN). We need to be careful to preserve
  // them.
  if (Permissions & all_write) {
    Attributes &= ~FILE_ATTRIBUTE_READONLY;
    if (Attributes == 0)
      // FILE_ATTRIBUTE_NORMAL indicates no other attributes are set.
      Attributes |= FILE_ATTRIBUTE_NORMAL;
  }
  else {
    Attributes |= FILE_ATTRIBUTE_READONLY;
    // FILE_ATTRIBUTE_NORMAL is not compatible with any other attributes, so
    // remove it, if it is present.
    Attributes &= ~FILE_ATTRIBUTE_NORMAL;
  }

  if (!::SetFileAttributesW(PathUTF16.begin(), Attributes))
    return mapWindowsError(GetLastError());

  return std::error_code();
}

std::error_code setLastModificationAndAccessTime(int FD, TimePoint<> Time) {
  FILETIME FT = toFILETIME(Time);
  HANDLE FileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(FD));
  if (!SetFileTime(FileHandle, NULL, &FT, &FT))
    return mapWindowsError(::GetLastError());
  return std::error_code();
}

std::error_code mapped_file_region::init(int FD, uint64_t Offset,
                                         mapmode Mode) {
  HANDLE FileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(FD));
  if (FileHandle == INVALID_HANDLE_VALUE)
    return make_error_code(errc::bad_file_descriptor);

  DWORD flprotect;
  switch (Mode) {
  case readonly:  flprotect = PAGE_READONLY; break;
  case readwrite: flprotect = PAGE_READWRITE; break;
  case priv:      flprotect = PAGE_WRITECOPY; break;
  }

  HANDLE FileMappingHandle =
      ::CreateFileMappingW(FileHandle, 0, flprotect,
                           Hi_32(Size),
                           Lo_32(Size),
                           0);
  if (FileMappingHandle == NULL) {
    std::error_code ec = mapWindowsError(GetLastError());
    return ec;
  }

  DWORD dwDesiredAccess;
  switch (Mode) {
  case readonly:  dwDesiredAccess = FILE_MAP_READ; break;
  case readwrite: dwDesiredAccess = FILE_MAP_WRITE; break;
  case priv:      dwDesiredAccess = FILE_MAP_COPY; break;
  }
  Mapping = ::MapViewOfFile(FileMappingHandle,
                            dwDesiredAccess,
                            Offset >> 32,
                            Offset & 0xffffffff,
                            Size);
  if (Mapping == NULL) {
    std::error_code ec = mapWindowsError(GetLastError());
    ::CloseHandle(FileMappingHandle);
    return ec;
  }

  if (Size == 0) {
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T Result = VirtualQuery(Mapping, &mbi, sizeof(mbi));
    if (Result == 0) {
      std::error_code ec = mapWindowsError(GetLastError());
      ::UnmapViewOfFile(Mapping);
      ::CloseHandle(FileMappingHandle);
      return ec;
    }
    Size = mbi.RegionSize;
  }

  // Close all the handles except for the view. It will keep the other handles
  // alive.
  ::CloseHandle(FileMappingHandle);
  return std::error_code();
}

mapped_file_region::mapped_file_region(int fd, mapmode mode, size_t length,
                                       uint64_t offset, std::error_code &ec)
    : Size(length), Mapping() {
  ec = init(fd, offset, mode);
  if (ec)
    Mapping = 0;
}

mapped_file_region::~mapped_file_region() {
  if (Mapping)
    ::UnmapViewOfFile(Mapping);
}

size_t mapped_file_region::size() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return Size;
}

char *mapped_file_region::data() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return reinterpret_cast<char*>(Mapping);
}

const char *mapped_file_region::const_data() const {
  assert(Mapping && "Mapping failed but used anyway!");
  return reinterpret_cast<const char*>(Mapping);
}

int mapped_file_region::alignment() {
  SYSTEM_INFO SysInfo;
  ::GetSystemInfo(&SysInfo);
  return SysInfo.dwAllocationGranularity;
}

static basic_file_status status_from_find_data(WIN32_FIND_DATAW *FindData) {
  return basic_file_status(file_type_from_attrs(FindData->dwFileAttributes),
                           perms_from_attrs(FindData->dwFileAttributes),
                           FindData->ftLastAccessTime.dwHighDateTime,
                           FindData->ftLastAccessTime.dwLowDateTime,
                           FindData->ftLastWriteTime.dwHighDateTime,
                           FindData->ftLastWriteTime.dwLowDateTime,
                           FindData->nFileSizeHigh, FindData->nFileSizeLow);
}

std::error_code detail::directory_iterator_construct(detail::DirIterState &it,
                                                     StringRef path,
                                                     bool follow_symlinks) {
  SmallVector<wchar_t, 128> path_utf16;

  if (std::error_code ec = widenPath(path, path_utf16))
    return ec;

  // Convert path to the format that Windows is happy with.
  if (path_utf16.size() > 0 &&
      !is_separator(path_utf16[path.size() - 1]) &&
      path_utf16[path.size() - 1] != L':') {
    path_utf16.push_back(L'\\');
    path_utf16.push_back(L'*');
  } else {
    path_utf16.push_back(L'*');
  }

  //  Get the first directory entry.
  WIN32_FIND_DATAW FirstFind;
  ScopedFindHandle FindHandle(::FindFirstFileExW(
      c_str(path_utf16), FindExInfoBasic, &FirstFind, FindExSearchNameMatch,
      NULL, FIND_FIRST_EX_LARGE_FETCH));
  if (!FindHandle)
    return mapWindowsError(::GetLastError());

  size_t FilenameLen = ::wcslen(FirstFind.cFileName);
  while ((FilenameLen == 1 && FirstFind.cFileName[0] == L'.') ||
         (FilenameLen == 2 && FirstFind.cFileName[0] == L'.' &&
                              FirstFind.cFileName[1] == L'.'))
    if (!::FindNextFileW(FindHandle, &FirstFind)) {
      DWORD LastError = ::GetLastError();
      // Check for end.
      if (LastError == ERROR_NO_MORE_FILES)
        return detail::directory_iterator_destruct(it);
      return mapWindowsError(LastError);
    } else
      FilenameLen = ::wcslen(FirstFind.cFileName);

  // Construct the current directory entry.
  SmallString<128> directory_entry_name_utf8;
  if (std::error_code ec =
          UTF16ToUTF8(FirstFind.cFileName, ::wcslen(FirstFind.cFileName),
                      directory_entry_name_utf8))
    return ec;

  it.IterationHandle = intptr_t(FindHandle.take());
  SmallString<128> directory_entry_path(path);
  path::append(directory_entry_path, directory_entry_name_utf8);
  it.CurrentEntry = directory_entry(directory_entry_path, follow_symlinks,
                                    status_from_find_data(&FirstFind));

  return std::error_code();
}

std::error_code detail::directory_iterator_destruct(detail::DirIterState &it) {
  if (it.IterationHandle != 0)
    // Closes the handle if it's valid.
    ScopedFindHandle close(HANDLE(it.IterationHandle));
  it.IterationHandle = 0;
  it.CurrentEntry = directory_entry();
  return std::error_code();
}

std::error_code detail::directory_iterator_increment(detail::DirIterState &it) {
  WIN32_FIND_DATAW FindData;
  if (!::FindNextFileW(HANDLE(it.IterationHandle), &FindData)) {
    DWORD LastError = ::GetLastError();
    // Check for end.
    if (LastError == ERROR_NO_MORE_FILES)
      return detail::directory_iterator_destruct(it);
    return mapWindowsError(LastError);
  }

  size_t FilenameLen = ::wcslen(FindData.cFileName);
  if ((FilenameLen == 1 && FindData.cFileName[0] == L'.') ||
      (FilenameLen == 2 && FindData.cFileName[0] == L'.' &&
                           FindData.cFileName[1] == L'.'))
    return directory_iterator_increment(it);

  SmallString<128> directory_entry_path_utf8;
  if (std::error_code ec =
          UTF16ToUTF8(FindData.cFileName, ::wcslen(FindData.cFileName),
                      directory_entry_path_utf8))
    return ec;

  it.CurrentEntry.replace_filename(Twine(directory_entry_path_utf8),
                                   status_from_find_data(&FindData));
  return std::error_code();
}

ErrorOr<basic_file_status> directory_entry::status() const {
  return Status;
}

static std::error_code directoryRealPath(const Twine &Name,
                                         SmallVectorImpl<char> &RealPath) {
  SmallVector<wchar_t, 128> PathUTF16;

  if (std::error_code EC = widenPath(Name, PathUTF16))
    return EC;

  HANDLE H =
      ::CreateFileW(PathUTF16.begin(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (H == INVALID_HANDLE_VALUE)
    return mapWindowsError(GetLastError());
  std::error_code EC = realPathFromHandle(H, RealPath);
  ::CloseHandle(H);
  return EC;
}

std::error_code openFileForRead(const Twine &Name, int &ResultFD,
                                SmallVectorImpl<char> *RealPath) {
  SmallVector<wchar_t, 128> PathUTF16;

  if (std::error_code EC = widenPath(Name, PathUTF16))
    return EC;

  HANDLE H =
      ::CreateFileW(PathUTF16.begin(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (H == INVALID_HANDLE_VALUE) {
    DWORD LastError = ::GetLastError();
    std::error_code EC = mapWindowsError(LastError);
    // Provide a better error message when trying to open directories.
    // This only runs if we failed to open the file, so there is probably
    // no performances issues.
    if (LastError != ERROR_ACCESS_DENIED)
      return EC;
    if (is_directory(Name))
      return make_error_code(errc::is_a_directory);
    return EC;
  }

  int FD = ::_open_osfhandle(intptr_t(H), 0);
  if (FD == -1) {
    ::CloseHandle(H);
    return mapWindowsError(ERROR_INVALID_HANDLE);
  }

  // Fetch the real name of the file, if the user asked
  if (RealPath)
    realPathFromHandle(H, *RealPath);

  ResultFD = FD;
  return std::error_code();
}

std::error_code openFileForWrite(const Twine &Name, int &ResultFD,
                            sys::fs::OpenFlags Flags, unsigned Mode) {
  // Verify that we don't have both "append" and "excl".
  assert((!(Flags & sys::fs::F_Excl) || !(Flags & sys::fs::F_Append)) &&
         "Cannot specify both 'excl' and 'append' file creation flags!");

  SmallVector<wchar_t, 128> PathUTF16;

  if (std::error_code EC = widenPath(Name, PathUTF16))
    return EC;

  DWORD CreationDisposition;
  if (Flags & F_Excl)
    CreationDisposition = CREATE_NEW;
  else if (Flags & F_Append)
    CreationDisposition = OPEN_ALWAYS;
  else
    CreationDisposition = CREATE_ALWAYS;

  DWORD Access = GENERIC_WRITE;
  DWORD Attributes = FILE_ATTRIBUTE_NORMAL;
  if (Flags & F_RW)
    Access |= GENERIC_READ;
  if (Flags & F_Delete) {
    Access |= DELETE;
    Attributes |= FILE_FLAG_DELETE_ON_CLOSE;
  }

  HANDLE H =
      ::CreateFileW(PathUTF16.data(), Access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CreationDisposition, Attributes, NULL);

  if (H == INVALID_HANDLE_VALUE) {
    DWORD LastError = ::GetLastError();
    std::error_code EC = mapWindowsError(LastError);
    // Provide a better error message when trying to open directories.
    // This only runs if we failed to open the file, so there is probably
    // no performances issues.
    if (LastError != ERROR_ACCESS_DENIED)
      return EC;
    if (is_directory(Name))
      return make_error_code(errc::is_a_directory);
    return EC;
  }

  int OpenFlags = 0;
  if (Flags & F_Append)
    OpenFlags |= _O_APPEND;

  if (Flags & F_Text)
    OpenFlags |= _O_TEXT;

  int FD = ::_open_osfhandle(intptr_t(H), OpenFlags);
  if (FD == -1) {
    ::CloseHandle(H);
    return mapWindowsError(ERROR_INVALID_HANDLE);
  }

  ResultFD = FD;
  return std::error_code();
}

std::error_code remove_directories(const Twine &path, bool IgnoreErrors) {
  // Convert to utf-16.
  SmallVector<wchar_t, 128> Path16;
  std::error_code EC = widenPath(path, Path16);
  if (EC && !IgnoreErrors)
    return EC;

  // SHFileOperation() accepts a list of paths, and so must be double null-
  // terminated to indicate the end of the list.  The buffer is already null
  // terminated, but since that null character is not considered part of the
  // vector's size, pushing another one will just consume that byte.  So we
  // need to push 2 null terminators.
  Path16.push_back(0);
  Path16.push_back(0);

  SHFILEOPSTRUCTW shfos = {};
  shfos.wFunc = FO_DELETE;
  shfos.pFrom = Path16.data();
  shfos.fFlags = FOF_NO_UI;

  int result = ::SHFileOperationW(&shfos);
  if (result != 0 && !IgnoreErrors)
    return mapWindowsError(result);
  return std::error_code();
}

static void expandTildeExpr(SmallVectorImpl<char> &Path) {
  // Path does not begin with a tilde expression.
  if (Path.empty() || Path[0] != '~')
    return;

  StringRef PathStr(Path.begin(), Path.size());
  PathStr = PathStr.drop_front();
  StringRef Expr = PathStr.take_until([](char c) { return path::is_separator(c); });

  if (!Expr.empty()) {
    // This is probably a ~username/ expression.  Don't support this on Windows.
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

namespace path {
static bool getKnownFolderPath(KNOWNFOLDERID folderId,
                               SmallVectorImpl<char> &result) {
  wchar_t *path = nullptr;
  if (::SHGetKnownFolderPath(folderId, KF_FLAG_CREATE, nullptr, &path) != S_OK)
    return false;

  bool ok = !UTF16ToUTF8(path, ::wcslen(path), result);
  ::CoTaskMemFree(path);
  return ok;
}

bool getUserCacheDir(SmallVectorImpl<char> &Result) {
  return getKnownFolderPath(FOLDERID_LocalAppData, Result);
}

bool home_directory(SmallVectorImpl<char> &result) {
  return getKnownFolderPath(FOLDERID_Profile, result);
}

static bool getTempDirEnvVar(const wchar_t *Var, SmallVectorImpl<char> &Res) {
  SmallVector<wchar_t, 1024> Buf;
  size_t Size = 1024;
  do {
    Buf.reserve(Size);
    Size = GetEnvironmentVariableW(Var, Buf.data(), Buf.capacity());
    if (Size == 0)
      return false;

    // Try again with larger buffer.
  } while (Size > Buf.capacity());
  Buf.set_size(Size);

  return !windows::UTF16ToUTF8(Buf.data(), Size, Res);
}

static bool getTempDirEnvVar(SmallVectorImpl<char> &Res) {
  const wchar_t *EnvironmentVariables[] = {L"TMP", L"TEMP", L"USERPROFILE"};
  for (auto *Env : EnvironmentVariables) {
    if (getTempDirEnvVar(Env, Res))
      return true;
  }
  return false;
}

void system_temp_directory(bool ErasedOnReboot, SmallVectorImpl<char> &Result) {
  (void)ErasedOnReboot;
  Result.clear();

  // Check whether the temporary directory is specified by an environment var.
  // This matches GetTempPath logic to some degree. GetTempPath is not used
  // directly as it cannot handle evn var longer than 130 chars on Windows 7
  // (fixed on Windows 8).
  if (getTempDirEnvVar(Result)) {
    assert(!Result.empty() && "Unexpected empty path");
    native(Result); // Some Unix-like shells use Unix path separator in $TMP.
    fs::make_absolute(Result); // Make it absolute if not already.
    return;
  }

  // Fall back to a system default.
  const char *DefaultResult = "C:\\Temp";
  Result.append(DefaultResult, DefaultResult + strlen(DefaultResult));
}
} // end namespace path
} // end namespace sys
} // end namespace llvm
