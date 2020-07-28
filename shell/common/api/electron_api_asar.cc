// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <vector>

#include "base/numerics/safe_math.h"
#include "shell/common/asar/archive.h"
#include "shell/common/asar/asar_util.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/object_template_builder.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/gin_helper/wrappable.h"
#include "shell/common/node_includes.h"
#include "shell/common/node_util.h"

namespace {

class Archive : public gin_helper::Wrappable<Archive> {
 public:
  static v8::Local<v8::Value> Create(v8::Isolate* isolate,
                                     const base::FilePath& path) {
    auto archive = std::make_unique<asar::Archive>(path);
    if (!archive->Init())
      return v8::False(isolate);
    return (new Archive(isolate, std::move(archive)))->GetWrapper();
  }

  static void BuildPrototype(v8::Isolate* isolate,
                             v8::Local<v8::FunctionTemplate> prototype) {
    prototype->SetClassName(gin::StringToV8(isolate, "Archive"));
    gin_helper::ObjectTemplateBuilder(isolate, prototype->PrototypeTemplate())
        .SetProperty("path", &Archive::GetPath)
        .SetMethod("getFileInfo", &Archive::GetFileInfo)
        .SetMethod("stat", &Archive::Stat)
        .SetMethod("readdir", &Archive::Readdir)
        .SetMethod("realpath", &Archive::Realpath)
        .SetMethod("copyFileOut", &Archive::CopyFileOut)
        .SetMethod("read", &Archive::Read)
        .SetMethod("readSync", &Archive::ReadSync);
  }

 protected:
  Archive(v8::Isolate* isolate, std::unique_ptr<asar::Archive> archive)
      : archive_(std::move(archive)) {
    Init(isolate);
  }

  // Returns the path of the file.
  base::FilePath GetPath() { return archive_->path(); }

  // Reads the offset and size of file.
  v8::Local<v8::Value> GetFileInfo(v8::Isolate* isolate,
                                   const base::FilePath& path) {
    asar::Archive::FileInfo info;
    if (!archive_ || !archive_->GetFileInfo(path, &info))
      return v8::False(isolate);
    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", info.size);
    dict.Set("unpacked", info.unpacked);
    dict.Set("offset", info.offset);
    return dict.GetHandle();
  }

  // Returns a fake result of fs.stat(path).
  v8::Local<v8::Value> Stat(v8::Isolate* isolate, const base::FilePath& path) {
    asar::Archive::Stats stats;
    if (!archive_ || !archive_->Stat(path, &stats))
      return v8::False(isolate);
    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", stats.size);
    dict.Set("offset", stats.offset);
    dict.Set("isFile", stats.is_file);
    dict.Set("isDirectory", stats.is_directory);
    dict.Set("isLink", stats.is_link);
    return dict.GetHandle();
  }

  // Returns all files under a directory.
  v8::Local<v8::Value> Readdir(v8::Isolate* isolate,
                               const base::FilePath& path) {
    std::vector<base::FilePath> files;
    if (!archive_ || !archive_->Readdir(path, &files))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, files);
  }

  // Returns the path of file with symbol link resolved.
  v8::Local<v8::Value> Realpath(v8::Isolate* isolate,
                                const base::FilePath& path) {
    base::FilePath realpath;
    if (!archive_ || !archive_->Realpath(path, &realpath))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, realpath);
  }

  // Copy the file out into a temporary file and returns the new path.
  v8::Local<v8::Value> CopyFileOut(v8::Isolate* isolate,
                                   const base::FilePath& path) {
    base::FilePath new_path;
    if (!archive_ || !archive_->CopyFileOut(path, &new_path))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, new_path);
  }

  v8::Local<v8::ArrayBuffer> ReadSync(gin_helper::ErrorThrower thrower,
                                      uint64_t offset,
                                      uint64_t length) {
    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > archive_->file()->length()) {
      thrower.ThrowError("Out of bounds read requested in ASAR");
      return v8::Local<v8::ArrayBuffer>();
    }
    auto array_buffer = v8::ArrayBuffer::New(thrower.isolate(), length);
    auto backing_store = array_buffer->GetBackingStore();
    memcpy(backing_store->Data(), archive_->file()->data() + offset, length);
    return array_buffer;
  }

  v8::Local<v8::Promise> Read(gin_helper::ErrorThrower thrower,
                              uint64_t offset,
                              uint64_t length) {
    gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise(thrower.isolate());
    v8::Local<v8::Promise> handle = promise.GetHandle();

    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > archive_->file()->length()) {
      thrower.ThrowError("Out of bounds read requested in ASAR");
      return v8::Local<v8::Promise>();
    }

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Archive::ReadOnIO, thrower.isolate(), archive_, offset,
                       length),
        base::BindOnce(&Archive::ResolveReadOnUI, std::move(promise)));

    return handle;
  }

 private:
  static std::unique_ptr<v8::BackingStore> ReadOnIO(
      v8::Isolate* isolate,
      std::shared_ptr<asar::Archive> archive,
      uint64_t offset,
      uint64_t length) {
    auto backing_store = v8::ArrayBuffer::NewBackingStore(isolate, length);
    memcpy(backing_store->Data(), archive->file()->data() + offset, length);
    return backing_store;
  }

  static void ResolveReadOnUI(
      gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise,
      std::unique_ptr<v8::BackingStore> backing_store) {
    v8::HandleScope scope(promise.isolate());
    v8::Context::Scope context_scope(promise.GetContext());
    auto array_buffer =
        v8::ArrayBuffer::New(promise.isolate(), std::move(backing_store));
    promise.Resolve(array_buffer);
  }

  std::shared_ptr<asar::Archive> archive_;

  DISALLOW_COPY_AND_ASSIGN(Archive);
};

void InitAsarSupport(v8::Isolate* isolate, v8::Local<v8::Value> require) {
  // Evaluate asar_bundle.js.
  std::vector<v8::Local<v8::String>> asar_bundle_params = {
      node::FIXED_ONE_BYTE_STRING(isolate, "require")};
  std::vector<v8::Local<v8::Value>> asar_bundle_args = {require};
  electron::util::CompileAndCall(
      isolate->GetCurrentContext(), "electron/js2c/asar_bundle",
      &asar_bundle_params, &asar_bundle_args, nullptr);
}

v8::Local<v8::Value> SplitPath(v8::Isolate* isolate,
                               const base::FilePath& path) {
  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  base::FilePath asar_path, file_path;
  if (asar::GetAsarArchivePath(path, &asar_path, &file_path, true)) {
    dict.Set("isAsar", true);
    dict.Set("asarPath", asar_path);
    dict.Set("filePath", file_path);
  } else {
    dict.Set("isAsar", false);
  }
  return dict.GetHandle();
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin_helper::Dictionary dict(context->GetIsolate(), exports);
  dict.SetMethod("createArchive", &Archive::Create);
  dict.SetMethod("splitPath", &SplitPath);
  dict.SetMethod("initAsarSupport", &InitAsarSupport);
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(electron_common_asar, Initialize)
