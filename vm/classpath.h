// Classpath utilities.

#ifndef CLASSPATH_H
#define CLASSPATH_H

#include "adt.h"
#include "util.h"
#include <stdlib.h>
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char *header; // pointer into the JAR memory
  u32 compressed_size;
  u32 claimed_uncompressed_size;
  bool is_compressed;
} jar_entry;

// JAR that's mapped into memory (or, on the web, fully downloaded and plopped
// into memory to simulate mmapping). Long-term we'll develop a system which
// lets us load chunks on demand.
typedef struct {
  // Map of complete file name to jar_entry
  string_hash_table entries;

  char *data;
  u32 size_bytes;
  bool is_mmap;    // true = mmap, false = heap allocation or WASMFS map
  bool needs_free; // false = WASMFS map
} mapped_jar;

typedef struct {
  // Fully qualified name to the file or folder
  heap_string name;
  // if this is a JAR, put it here; otherwise it's a folder
  mapped_jar *jar;
} classpath_entry;

// Classes will be sought for in JARs and folders in the order they're added.
typedef struct {
  classpath_entry *entries;
  heap_string as_colon_separated;
  // entries[0..runtime_entries_count) がランタイム（jdk23.jar 相当）由来。
  // これ以降はアプリケーション側のクラスパスで、リソース検索やシステムクラスローダ
  // の判定に使う
  u32 runtime_entries_count;
} classpath;

// Try to initialize the classpath using the colon-separated data in "path". Supported entries are JAR files and
// folders. (TODO index folders instead of rescanning each time)
// Returns nullptr if all elements in the path were loaded ok. Otherwise, returns a heap-allocated error message that
// is the caller's responsibility to free.
[[nodiscard]] char *init_classpath(classpath *cp, slice path);

// 実行中にエントリを1件追加する。失敗時はヒープ確保したエラーメッセージを返す
[[nodiscard]] char *classpath_add_entry(classpath *cp, slice entry);

// Free the classpath. (Does not call free(cp).)
void free_classpath(classpath *cp);

// Look for a classfile in the classpath.
// Example usage:
//   u8 *bytes; size_t len;
//   int failed = lookup_classpath(&cp, STR("java/lang/Object.class"), &bytes, &len);
int lookup_classpath(classpath *cp, slice filename, u8 **bytes, size_t *len);

// lookup_classpath に、ヒットしたクラスパスエントリの番号を返すものを足したもの
int lookup_classpath_idx(classpath *cp, slice filename, u8 **bytes, size_t *len, int *matched_index);

// jar 内の特定エントリだけを読む（マニフェストなど、クラスパス順に関係なく「この jar」の中身を見たいとき用）
enum jar_lookup_result { JAR_NOT_FOUND, JAR_FOUND, JAR_CORRUPT /* e.g. if INFLATE fails */ };
enum jar_lookup_result jar_lookup(mapped_jar *jar, slice filename, u8 **bytes, size_t *len);

// クラスファイル以外のリソース（META-INF/... など）をクラスパス全エントリから探す。
// ディレクトリエントリでは存在確認した実パスを、jar エントリでは out_dir 配下に
// 展開したファイルのパスを返す。戻り値は heap_string の stb 配列（呼び出し側が
// arrfree + free_heap_str で解放する）。見つからなければ空配列
heap_string *classpath_find_resources(classpath *cp, slice filename);

#ifdef __cplusplus
}
#endif

#endif // CLASSPATH_H
