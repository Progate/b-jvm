#include <natives-dsl.h>
#include <zlib.h>

// java.util.zip.Deflater。戻り値の詰め方と params の読み方は JDK の
// src/java.base/share/native/libzip/Deflater.c（doDeflate / checkDeflateStatus）と同じ。
// zip を書き出す道（jdk.zipfs の newOutputStream、JarOutputStream など）がここを通る

DECLARE_NATIVE("java/util/zip", Deflater, initIDs, "()V") { return value_null(); }

DECLARE_NATIVE("java/util/zip", Deflater, init, "(IIZ)J") {
  z_stream *stream = calloc(1, sizeof(z_stream));
  if (!stream) {
    out_of_memory(thread);
    return value_null();
  }
  int ret = deflateInit2(stream, args[0].i, Z_DEFLATED, args[2].i ? -MAX_WBITS : MAX_WBITS, 8, args[1].i);
  if (ret != Z_OK) {
    free(stream);
    raise_vm_exception(thread, STR("java/lang/InternalError"), STR("deflateInit2 failed"));
    return value_null();
  }
  return (stack_value){.l = (s64)stream};
}

DECLARE_NATIVE("java/util/zip", Deflater, setDictionary, "(J[BII)V") {
  z_stream *stream = (z_stream *)args[0].l;
  deflateSetDictionary(stream, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Deflater, setDictionaryBuffer, "(JJI)V") {
  deflateSetDictionary((z_stream *)args[0].l, (u8 *)args[1].l, args[2].i);
  return value_null();
}

static stack_value do_deflate(vm_thread *thread, s64 addr, u8 *input, s32 input_len, u8 *output, s32 output_len,
                              s32 flush, s32 params) {
  z_stream *stream = (z_stream *)addr;
  bool set_params = params & 1;
  stream->next_in = input;
  stream->avail_in = input_len;
  stream->next_out = output;
  stream->avail_out = output_len;
  int res = set_params ? deflateParams(stream, params >> 3, (params >> 1) & 3) : deflate(stream, flush);

  s64 input_used = 0, output_used = 0;
  bool finished = false;
  if (set_params) {
    switch (res) {
    case Z_OK:
      set_params = false;
      [[fallthrough]];
    case Z_BUF_ERROR:
      input_used = input_len - stream->avail_in;
      output_used = output_len - stream->avail_out;
      break;
    default:
      raise_vm_exception(thread, STR("java/lang/InternalError"), STR("deflateParams failed"));
      return value_null();
    }
  } else {
    switch (res) {
    case Z_STREAM_END:
      finished = true;
      [[fallthrough]];
    case Z_OK:
      input_used = input_len - stream->avail_in;
      output_used = output_len - stream->avail_out;
      break;
    case Z_BUF_ERROR:
      break;
    default:
      raise_vm_exception(thread, STR("java/lang/InternalError"), STR("deflate failed"));
      return value_null();
    }
  }
  return (stack_value){.l = input_used | output_used << 31 | (s64)finished << 62 | (s64)set_params << 63};
}

DECLARE_NATIVE("java/util/zip", Deflater, deflateBytesBytes, "(J[BII[BIIII)J") {
  return do_deflate(thread, args[0].l, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i,
                    (u8 *)ArrayData(args[4].handle->obj) + args[5].i, args[6].i, args[7].i, args[8].i);
}

DECLARE_NATIVE("java/util/zip", Deflater, deflateBytesBuffer, "(J[BIIJIII)J") {
  return do_deflate(thread, args[0].l, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i, (u8 *)args[4].l,
                    args[5].i, args[6].i, args[7].i);
}

DECLARE_NATIVE("java/util/zip", Deflater, deflateBufferBytes, "(JJI[BIIII)J") {
  return do_deflate(thread, args[0].l, (u8 *)args[1].l, args[2].i, (u8 *)ArrayData(args[3].handle->obj) + args[4].i,
                    args[5].i, args[6].i, args[7].i);
}

DECLARE_NATIVE("java/util/zip", Deflater, deflateBufferBuffer, "(JJIJIII)J") {
  return do_deflate(thread, args[0].l, (u8 *)args[1].l, args[2].i, (u8 *)args[3].l, args[4].i, args[5].i, args[6].i);
}

DECLARE_NATIVE("java/util/zip", Deflater, getAdler, "(J)I") {
  return (stack_value){.i = (s32)((z_stream *)args[0].l)->adler};
}

DECLARE_NATIVE("java/util/zip", Deflater, reset, "(J)V") {
  deflateReset((z_stream *)args[0].l);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Deflater, end, "(J)V") {
  z_stream *stream = (z_stream *)args[0].l;
  deflateEnd(stream);
  free(stream);
  return value_null();
}

// Adler32 と CRC32 の残り（バイト 1 つ・ByteBuffer）
DECLARE_NATIVE("java/util/zip", Adler32, update, "(II)I") {
  u8 b = (u8)args[1].i;
  return (stack_value){.i = (s32)adler32((uLong)(u32)args[0].i, &b, 1)};
}

DECLARE_NATIVE("java/util/zip", Adler32, updateBytes, "(I[BII)I") {
  return (stack_value){
      .i = (s32)adler32((uLong)(u32)args[0].i, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i)};
}

DECLARE_NATIVE("java/util/zip", Adler32, updateByteBuffer, "(IJII)I") {
  return (stack_value){.i = (s32)adler32((uLong)(u32)args[0].i, (u8 *)args[1].l + args[2].i, args[3].i)};
}

DECLARE_NATIVE("java/util/zip", CRC32, update, "(II)I") {
  u8 b = (u8)args[1].i;
  return (stack_value){.i = (s32)crc32((uLong)(u32)args[0].i, &b, 1)};
}

DECLARE_NATIVE("java/util/zip", CRC32, updateByteBuffer0, "(IJII)I") {
  return (stack_value){.i = (s32)crc32((uLong)(u32)args[0].i, (u8 *)args[1].l + args[2].i, args[3].i)};
}
