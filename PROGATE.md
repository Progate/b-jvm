# Progate/b-jvm（`3fd56c7+progate`）

[anematode/b-jvm](https://github.com/anematode/b-jvm) を、BrowserOS（WASI、のちに wasm32-linux-musl）で動かすための変更を加えた fork。
[Progate/packages](https://github.com/Progate/packages) の `@progate/browser-java` が、このブランチのコミットを固定して取って組む。

- 元にした upstream のコミット: `3fd56c7`（"get rid of the buffering"）
- 変更は 1 つずつのコミットとして積んである。以前は Progate/packages の `packages/browser-java/vendor/b-jvm` に
  取り込んでいたもので、その履歴をコミットに積み直した
- ライセンス: `LICENSE` を参照

## 加えた変更

`WASI`（wasm32）をネイティブ・Emscripten に次ぐ3つ目のターゲットとして追加している。要点だけ:

- `vm/types.h` の `BJVM_WASM` … 「wasm をターゲットにしている」と「Emscripten ランタイムで動く」の分離。
  `interpreter2.c` は前者の意味で `EMSCRIPTEN` を見ていた（tail call ディスパッチ・インライン asm・
  飽和変換）ため、そのままでは wasi ビルドが x86 の経路を選んでコンパイルできなかった
- `cmake/WasiSupport.cmake` … SjLj（`-mllvm -wasm-enable-sjlj` + `-lsetjmp`）・
  WASI の mman エミュレーション（`-lwasi-emulated-mman`）・reactor リンク
  （`-mexec-model=reactor` + `--export-dynamic`）
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … 同梱 JDK の `UnixConstants` は Linux 生成なので、
  oflags を wasi-libc の値へ変換する。また wasi-libc の mmap は malloc ベースでアラインしないため
  `allocationGranularity0` を 1 にする（4096 のままだと `FileChannelImpl` の assert に落ち、
  boot layer の初期化が失敗する）
- `natives/share/java/io/posix-fs.c` … wasi-libc は `__unix__` を定義しないが `stat(2)` は持つので
  POSIX 経路を使う。ついでに未サポート側の分岐にあったシグネチャの誤りを修正
- `natives/share/java/io/FileInputStream.c` … `available0` の `ioctl(FIONREAD)` が失敗したとき、
  通常ファイルなら「サイズ - 現在位置」で代替する。**preview2 の wasi-libc は FIONREAD を
  実装していない**（preview1 は成功するが常に 0 を返す）。元は失敗を即 `IOException` にしていたため、
  preview2 ではクラスパスからのリソース読み込みがまるごと死に、ECJ が
  `Missing resource : org/eclipse/jdt/...` で止まっていた。preview1 では `ioctl` が成功するので挙動は変わらない

wasi 対応そのものとは別に、ブラウザー内で Java をコンパイルできるようにするための穴埋めも入っている。
ECJ（Eclipse の batch コンパイラ）を動かそうとして見つかったもので、いずれも実 JDK との差:

- `natives/share/jdk/internal/util/SystemProps.c` … `jdk.boot.class.path.append` を申告する。
  VM はアプリのクラスも bootstrap ローダーで読むが、BootLoader は**リソース**をこのプロパティ
  からしか探さない。そのため VM 自身が読めているクラスの `getResourceAsStream` が null を返していた
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … `UnixFileDispatcherImpl` の
  read0/pread0/write0/pwrite0/seek0/truncate0/force0/close0。mmap 側しか実装されておらず、
  `Files.newInputStream` など java.nio 経由が全部 unsatisfied link error になっていた
- `natives/share/java/io/UnixFileSystem.c` … `list0`（`File.list()`）
- `natives/share/java/lang/Runtime.c` + `vm/bjvm.h` … `Shutdown.halt0` が no-op で
  `System.exit` の終了コードが取れなかった。記録して `ffi_take_exit_status` で公開する
- `vm/bin/main.c` … `ffi_create_string_array`（`main(String[])` の引数）と `ffi_take_exit_status`

- `natives/share/java/lang/Module.c` + `vm/bjvm.c` … `Module.defineModule0` が受け取る package の一覧を
  `vm->packages` に記録し、`define_class` でクラスをその package を持つモジュールへ割り当てる。元は
  bootstrap で読んだクラスをすべて java.base 所属にしていたため、`Class.forName(Module, name)` が
  「モジュールが違う」として null を返し、ServiceLoader のモジュール由来の provider
  （jdk.localedata の `CLDRLocaleDataMetaInfo` など）を jar にクラスがあっても
  "Provider … not found" で落としていた。宣言の無い package は従来どおり java.base / 無名モジュール

実行時の警告を消すための変更も入っている。

- `natives/share/java/lang/ProcessEnvironment.c` … `ProcessImpl.forkAndExec` の
  `Warning: tried to fork and exec …` を消した。ブラウザー内ではプロセスを spawn できないが、
  JUnit（jline が端末サイズを取るために `stty` を exec する）のように出力が汚れるだけの呼び出しがあり、
  warning を出しても利用者には直せないため
- `vm/classfile.c` + `vm/classfile.h` + `natives/share/java/lang/Class.c` …
  `InnerClasses` 属性の inner name を `attribute_inner_classes.names` に保持するようにし、
  HotSpot の `JVM_GetSimpleBinaryName` と同じ意味で `Class.getSimpleBinaryName0` を実装。
  元は未実装で、呼ばれるたびに `Unsatisfied link error getSimpleBinaryName0` が stderr に出ていた
  （anonymous / トップレベル / 配列 / primitive では null を返すのが正しい挙動）

完全な差分は b-jvm 側の `feat/wasi-target` ブランチ（`e4cae5d` 〜 `94a22df`）にある。

## Spring Boot を動かすための変更（2026-09 追加分）

Tomcat 10.1（NIO コネクタ）まで含めて exploded classpath な Spring Boot アプリが
HTTP 応答できることを確認した差分。`browser-wasi` の socket ABI
（`__wasi_sock_*`、browser-os が用意する p1 拡張）へ橋渡ししている。

- `natives/share/sun/nio/ch/` 配下のソケット系ネイティブを一括追加:
  - `Net.c` … `socket0/bind0/connect0/accept0/poll0/localOrRemoteInetAddress/available`
    など。`accept`/`poll` は async native で yield しながら WASI fd をポーリングする
    （ブロッキング read を持ち回りスケジューラが止まらないようにするため）。
    `socket0` は `stream=false` で SOCK_DGRAM（UDP）も開く。`bind4/connect4` は
    holder の big-endian int をネットワーク順バイトに直して ABI へ渡す
    （そのまま渡すと 127.0.0.1 が 1.0.0.127 に化けて宛先マッチしない）
  - `SocketDispatcher.c` … `read0/readv0/write0/writev0/pread0`。非ブロッキング fd は
    `IOStatus.UNAVAILABLE`（EAGAIN）をそのまま返し、ブロッキングなら yield-poll で待つ
  - `UnixDispatcher.c` … `close0/preClose0`（fake fd も含めて解放）
  - `DatagramDispatcher.c` … `read0/readv0/write0/writev0`。接続済みデータグラム
    fd は `fd_read`/`fd_write` に載るので SocketDispatcher と同じ yield-poll 形
  - `DatagramChannelImpl.c` … `receive0/send0/disconnect0`。`sock_recv_from`/
    `sock_send_to` ABI へ橋渡し。JDK の NativeSocketAddress（本物の sockaddr_in
    レイアウト）と ABI の {family u16, addr @+2, port 別} 形式を毎回相互変換する。
    `disconnect0` は表に相当操作が無いので no-op
  - `EPoll.c` + `EventFD.c` … JDK の `EPollSelectorImpl` が使う epoll/eventfd を
    fake fd（`bjvm_fakefd`）として実装。`ctl` は登録 fd の集合だけを管理し、`wait` は
    各 fd を `poll_oneoff` で順に覗いて 8byte ずつ `{events, fd}` を pollArray に書く
  - `IOUtil.c` … `drain`（eventfd のカウンタを読み切る）/`configureBlocking`
    （fd を非ブロッキング表 `bjvm_nonblock_fds` に登録）/`iovMax`/`setOption`/`getOption`
    /`connectionReset`/`initIDs`/`spinit` など
  - `NativeSocketAddress.c` … `check/describe/checkRemote`
  - `FileDispatcherImpl.c` + `UnixFileDispatcherImpl.c` … `FileDispatcherImpl` は
    `init/append0`、ソケットを流れる `FileDescriptor` の `fd_table` 参照の穴埋め
  - `bjvm_net.h` + `bjvm_net.c` … fakefd テーブル・非ブロッキング fd 集合・
    `bjvm_fd_val`/`bjvm_fd_is_nonblock`/`bjvm_fd_poll_once`・sockaddr ↔
    `InetSocketAddress` 変換の共有実装。**共有テーブルは 1 つの TU に置く**
    （common ヘッダに `static` で持たせると翻訳単位ごとに別コピーになり、
    `configureBlocking` で登録した非ブロッキングフラグが他の TU から見えない）
- `java/bjvm/SystemClassLoader.java` + `natives/share/bjvm/SystemClassLoader.c`
  + `vm/bjvm.c` … アプリケーションクラスローダの実装。クラスパス由来のクラスに
  `is_app_class` を立てて `Class.getClassLoader()` が本物のローダを返すようにし、
  `ClassLoader.scl` とスレッドの `contextClassLoader` を張る。Spring は
  `ClassLoader.getResources`/`contextClassLoader` 経由で jar 内の
  `META-INF/spring.factories` 等を列挙するため必須
- `vm/classpath.c` … アプリケーションクラスパス（マウントしたディレクトリ）からの
  `.class` 解決と `lib/*.jar` の列挙に対応
- `natives/share/java/lang/Thread.c` … `start0` の `run` 検索を
  `method_lookup(..., true, true)`（スーパークラスを辿る）に修正。`run` を宣言しない
  `Thread` サブクラス（Tomcat の `TaskThread` 等）は nullptr を解決してしまい、
  null メソッド実行が（wasm のゼロ読みで）空の `cp_method` を介して
  `AbstractMethodError` になり、例外は execution record に飲まれてスレッドが
  **無言で死んでいた**。executor のワーカーが全滅して HTTP リクエストが処理されない
  原因だった
- `natives/share/java/lang/reflect/Array.c` … `Array.newInstance`（object 配列）
- `natives/share/java/lang/StackWalker.c` … `getCallerClass` 系の穴埋め
- `natives/share/java/net/*`（Inet4AddressImpl/Inet6AddressImpl/NetworkInterface）
  + `natives/share/sun/management/VMManagementImpl.c` … Tomcat/Spring の起動が要求する
  リンク先の穴埋め（v6 非対応・固定値でよい所はスタブ）
- `natives/share/java/lang/ref/Finalizer.c` … `Reference.refersTo0` がレシーバを
  `(void *)obj`（`handle*` のまま）として `struct native_Reference` にキャストしていた
  upstream バグを `obj->obj` に修正。`referent` フィールドが handle 構造体の隣接領域を
  読み、たまたま引数の key を指して常に true を返していたため、ThreadLocalMap の
  プローブが任意の entry を「一致」と誤判定して `Entry.value` が別キーの値で上書き・
  返却されていた（Spring Boot 初回リクエストで `DispatcherServlet$$Lambda` を
  `String` に CCE していた原因）
- `src/index.ts` / `src/vm.ts` … `sockets` バックエンドを WASI へ流すオプションと、
  非デーモンスレッドが残る限りスケジューラをポンプし続ける処理

## kotlinc を動かすための変更（2026-10 追加分）

本物の kotlinc 2.2.20（`K2JVMCompiler`）が hello.kt をコンパイルし、出来た `.class` を
同じ VM で走らせるまでに塞いだ差分。kotlinc は javac が出さない形（`nop`・インターフェースの
ひし形を通る default メソッド・`@CallerSensitive` の反射）を多く使うので、いずれも
実 JDK（HotSpot）や JVMS との差として見つかった。

- `vm/classfile.h` + `vm/interpreter2.c` + `vm/pretty_print.c` … 命令の種類 0 を空き
  （`insn_reserved_zero`）にした。notco の解釈器は「次の命令の種類 × 4 + TOS の型」を
  戻り値にし、0 を例外・中断の合図に使う。`nop` が 0 だと TOS が空の `nop` がこの合図と
  同じ値になり、メソッドがその場で抜けて null を返していた（kotlinc は inline 関数の境目に
  `nop` を出す。`K2JVMCompiler.createArguments()` が null を返していた）
- `vm/bjvm.c` + `vm/bjvm.h` … クラス初期化で先に初期化する親インターフェースを JVMS §5.5
  どおり「クラスのときだけ・default メソッドを持つものだけ」にした。インターフェースの初期化で
  親インターフェースまで初期化していたため、子の `<clinit>` の途中で親の `<clinit>` が走り、
  子の static 欄を null で読んでいた（kotlin-reflect の `KotlinTypeChecker` が
  `NewKotlinTypeChecker.Companion` を読む）
- `vm/interpreter2.c` … `getstatic` / `putstatic` が初期化するクラスを、参照先ではなく
  **欄を宣言したクラス**にした（JVMS §6.5）。上の変更で、親インターフェースから継いだ欄を
  子クラス経由で読むと初期化されないまま読めてしまうため
- `vm/vtable.c` … itable の曖昧判定を JVMS §5.4.3.3 の maximally-specific に合わせた。
  同じ名前・記述子の宣言が別のインターフェースから届くと一律に曖昧としていたため、
  ひし形を通って子インターフェースの default が届く形で `AbstractMethodError` になっていた
- `natives/share/java/lang/invoke/MethodHandleNatives.c` + `vm/cached_classdescs.h` …
  `resolve` の失敗を、HotSpot と同じく `speculativeResolve` なら null、それ以外は
  `NoSuchMethodError` / `NoSuchFieldError` にした（印も消さない）。素の `LinkageError` だと
  `MemberName.makeAccessException` が `IllegalAccessException` を選び、`@CallerSensitive` の
  メソッドを `Method.invoke` するとアダプター探しが `InternalError` で落ちていた
- `natives/share/jdk/internal/reflect/Reflection.c` … `getCallerClass` が飛ばすフレームを
  HotSpot の `is_ignored_by_security_stack_walk` 相当（`Method.invoke`・`jdk/internal/reflect`・
  signature polymorphic・LambdaForm）にした。`java/lang/invoke` を丸ごと飛ばしていたため、
  `MethodHandleImpl$BindCaller.checkCallerClass` が落ちていた
- `natives/share/java/lang/Throwable.c` … `StackTraceElement` のクラス名を binary name
  （ドット区切り）にした。`kotlin.jvm.internal.Intrinsics` は `getStackTrace()` の中から
  自分を `getClassName()` で探す。あわせて SourceFile の無いクラスの `fileName` に
  メソッド名が入っていたのを null にした
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … wasi-libc の errno を同梱 JDK の
  `UnixConstants`（Linux）の値へ読み替える（`stat0` の戻り値と `UnixException`）。
  `strerror` は逆に戻す。読み替えないと `NoSuchFileException` にならず素の
  `FileSystemException` になる。`lstat0` が失敗を握り潰していたのを `UnixException` に、
  `realpath0` を追加、`unmap0` の記述子を JDK 23 の `(JJ)I` に直した
- `natives/share/java/io/RandomAccessFile.c` … `FILE*` ではなく `FileDescriptor.fd`
  （整数の fd）で書き直した。`getChannel()` の FileChannel が同じ fd を使うため、元は
  `size` / `read` / `map` が `Bad file descriptor` だった。モード（`"rw"`）を無視して常に
  読み取りで開いていた・`length()` が位置を末尾へ動かしていた・EOF で -1 を返さなかった
  のも直し、`write0` / `writeBytes0` / `setLength0` を足した
- `natives/share/java/io/FileOutputStream.c` … `open0` を追加（ファイルへの書き出し）
- `natives/share/sun/management/VMManagementImpl.c` + `ThreadImpl.c` … スレッドの CPU 時間に
  対応していると答え、値は単調時計で返す。kotlinc は条件なしに `setThreadCpuTimeEnabled(true)` を呼ぶ
- `java/bjvm/SystemClassLoader.java` … 親を platform ローダーにした（本物の AppClassLoader と同じ）。
  ServiceLoader はローダーから親をたどって service 表を見るので、親が null だと platform ローダー所属の
  モジュールの provider（jdk.zipfs の `ZipFileSystemProvider`）が見つからず、
  `FileSystems.newFileSystem(zip)` が `ProviderNotFoundException` になっていた。kotlinc は klib をこれで開く
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … `access0` は JDK 23 では errno を戻り値で返す。
  例外を投げていたため、`Files.createDirectories` が親をたどる途中で生の `UnixException` が漏れていた
- `vm/interpreter2.c` … 例外ハンドラーの catch 型を、投げられた例外を退避してから解決する。
  ブートストラップでないローダー（platform ローダーが定義した jdk.zipfs のクラス）は Java の
  `loadClass` で解決するが、例外が保留中だと即座に失敗し、その例外が解決の失敗としてキャッシュされて
  ハンドラーが飛ばされていた（`ZipFileSystemProvider.ensureFile` の `catch (IOException)` が効かず、
  新しい zip を `create` で作れなかった）
- `natives/share/java/util/zip/Deflater.c`（新規）+ `Inflater.c` … `Deflater` の全ネイティブ、
  `Inflater` の ByteBuffer 版・辞書・`getAdler`、`CRC32` の 1 バイト版と ByteBuffer 版、`Adler32`。
  戻り値の詰め方は JDK の `Deflater.c`（`checkDeflateStatus`）と同じ。zip の書き出しが
  `UnsatisfiedLinkError` だった。`Inflater` が `Z_BUF_ERROR` / `Z_NEED_DICT` を「終わった」と返していたのも直した
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … JDK 23 が宣言していて無かったものを足した
  （`unlink0` / `rename0` / `rmdir0`（`rmdir` という名前で登録されていた）/ `link0` / `symlink0` /
  `readlink0` / `fstat0` / `fstatat0` / `openat0` / `unlinkat0` / `renameat0` / `fdopendir` /
  `rewind` / `read0` / `write0` / `close0` / 時刻の設定 4 種）。WASI に無いもの（chmod・chown・
  利用者名・xattr・statvfs・dup）は、権限の無い環境と同じ振る舞い（変えたことにする・`user` を返す・
  `ENOTSUP`）にした。`AT_*` は Linux の値から読み替える。`lstat0` が `stat` を呼んでいた
  （リンクの先の属性を返していた）のも直した

## Compose をコンパイルするための変更（2026-10 追加分）

kotlinc（wasm バックエンド + Compose のコンパイラプラグイン）で Compose Multiplatform の
アプリを klib にするところで見つかった差分。いずれも、ホストの JVM との差分テスト
（同じプログラムの出力・クラス初期化の順番 `-Xlog:class+init` を並べる）で特定した。

- `vm/interpreter2.c` … `ishr` の結果を `(u32)` にしていたのを `(s32)` にした。int は TOS に
  符号拡張した 64 ビットで持つ（`i2l` はそれを前提にそのまま広げる）ので、負の数を右シフトした値を
  long に広げると 4294967295 などに化けていた
- `vm/interpreter2.c` … `ldiv` / `lrem` の `MIN / -1` の判定を `INT64_MIN` にした。wasm32 では
  `long` が 32 ビットなので `LONG_MIN` は `INT32_MIN` で、`Long.MIN_VALUE / -1` が `i64.div_s` で
  トラップして VM ごと落ちていた
- `vm/bjvm.c` … MethodHandle 定数（`resolve_mh_mt` / `resolve_mh_invoke` / `resolve_mh_vh`）の解決で
  参照先のクラスを初期化しないようにした（JVMS §5.4.3.5）。初期化は `DirectMethodHandle.make` が作る
  MethodHandle が呼ばれたときに起きる。`Lazy::new` のラムダを作っただけで `Lazy` の `<clinit>` が走っていた
- `natives/share/java/lang/reflect/Array.c` … `Array.newInstance` で要素のクラスを初期化しないようにした
  （`anewarray` と同じ）。kotlinc では `Array.newInstance(KtProperty.class, 0)` が `KtProperty` の
  初期化から `KtNodeTypes` の初期化まで連鎖させ、`KtStubElementTypes` の初期化の途中の null を
  `KtNodeTypes.DOT_QUALIFIED_EXPRESSION` に入れていた。その結果、light tree からの変換が
  ドット付きの式（`"abc".length`・`16.dp`）を式と認識せず、`IrErrorType` になっていた

## Kotlin 2.4 のコンパイラを動かすための変更（2026-10 追加分）

- `vm/interpreter2.c` … null の `checkcast` / `instanceof` ではクラスを解決しない（HotSpot と同じ）。
  IntelliJ の `FileUtil` は、無い trove の型（`gnu.trove.TObjectHashingStrategy`）への `checkcast` に
  null を通す作りで、解決しに行くと static 初期化ごと `ClassNotFoundException` で落ちていた
- `natives/share/java/io/FileDescriptor.c` … `FileCleanable.cleanupClose0`（GC に回収された
  ファイルを閉じる）。無いと Cleaner のスレッドで `UnsatisfiedLinkError` が出る


## kotlinc を BrowserOS のコマンドにするための変更（2026-10 追加分）

`/usr/bin/kotlinc`（`java` を起動するシェルスクリプト）を、本物の JVM と同じ引数で動かすための差分。

- `vm/bin/main.c` + `natives/share/jdk/internal/util/SystemProps.c` … WASI 版は起動時に `PWD` へ
  `chdir` する（BrowserOS の決まり。→ browser-os の `src/wasi/imports.ts`）。移らないと Java の相対パスが
  すべて `/` 基準になり、`kotlinc main.kt` が `main.kt` を見つけられなかった（javac はパッケージ側で
  引数を絶対パスに直して回避していた）。そのため `java.home` は cwd から決めず `/jdk` に固定した。版を名に入れないのは、JDK を上げるたびに wasm の作り直しが要る箇所を増やしたくないためである
- `vm/classpath.c` + `natives/share/bjvm/SystemClassLoader.c` + `java/bjvm/SystemClassLoader.java` …
  jar の中のリソースを、`/bjvm-resources/` に展開した `file:` URL ではなく、本物の AppClassLoader と同じ
  `jar:file:/…/x.jar!/<名前>` で返す。kotlinc はリソースの URL から自分の jar の場所を逆算して拡張点の
  設定（`META-INF/extensions/compiler.xml`）を読むので、展開した URL では `-Xintellij-plugin-root`
  （2.4.20 で非推奨）を渡すしかなかった
- `natives/share/org/fusesource/jansi/internal/CLibrary.c` … jansi の `isatty`（端末ではないと答える）。
  kotlinc が起動するたびに `Unsatisfied link error isatty` が標準エラーに出ていた

- `natives/share/java/lang/Runtime.c` + `vm/bin/main.c` … `Shutdown.halt0` で本当に止まるようにした。
  終了コードを記録したあと、呼んだスレッドを二度と再開しない（起きる時刻 0 の sleep）。止めるのは
  ホスト側の駆動ループで、新しい `ffi_exit_requested` を見て回すのをやめる（→ browser-java の `src/vm.ts`）。
  以前は記録だけして戻っていたので `System.exit` のあともコードが走り続け、kotlinc はそれを検出して
  `System.exit returned normally` の例外を投げ、コンパイルエラーのたびにスタックが出ていた

## kotlinc を速くするための変更（2026-10 追加分）

- `vm/interpreter2.c` + `vm/classfile.h` + `vm/bjvm.c` … 呼び出し元で済ませる intrinsic
  （`cp_method.call_intrinsic`。クラスのリンク時に `find_call_intrinsic` が決める）。`String.equals` と
  Kotlin の `Intrinsics.checkNotNull*` / `areEqual` は、フレームを積まずに invoke の命令の中で結果を出す。
  b-jvm はインタプリタだけなので、1 回の呼び出し（フレームを積んで戻る）が本体より高く、kotlinc の
  リンクはサンプルの 2 割がこの 2 つだった。intrinsic は扱えない入力（null で例外を投げる検査・
  自前の `equals` を持つクラス）を断り、そのときは本物のメソッドを呼ぶ。`String.equals` は
  402ns → 37ns、JDK の起動も 3.0 秒 → 1.8 秒になった
- 同じ仕組みで、`aload_0; getfield; xreturn` だけのメソッド（getter。getfield が解決済みになってから）・
  `Preconditions.checkIndex`・`ArrayList$Itr.hasNext` / `checkForComodification` も呼び出し元で済ませる。
  例外になる入力（範囲外・反復中の変更・null）は本物のメソッドへ回す。synchronized の getter は対象外
- `natives/share/java/lang/Throwable.c` … `Thread.getStackTrace0`（他のスレッドのスタックを取る）。
  サンプリングで kotlinc の遅いところを探すのに使った
- `vm/wasi_sbrk.c` … wasi-libc の `sbrk` を置き換え、メモリを 64MB ずつまとめて伸ばす。wasi-libc の malloc
  （dlmalloc）は小刻みに `sbrk` を呼び、wasi-libc の `sbrk` はそのたびに `memory.grow` する。メモリを伸ばすのは
  ホストにとって安くなく（古い ArrayBuffer を切り離し、JS 側の GC も走る）、JDK と kotlinc のクラスを読む間に
  何万回も起きて、kotlinc の時間の 3 割を占めていた（kotlinc で hello.kt 1 つ: 10.3 秒 → 5.3 秒）

## JIT（2026-10 追加分）

upstream の `dumb_jit.c` は途中で止まっている（中身の多くが `nullptr` を返す）ので使わず、`vm/jit.c` を新しく
書いた。wasm32-linux-musl のビルドだけで動く。

- 熱くなったメソッド（インタプリタに入った回数が `BJVM_JIT_THRESHOLD`、既定 1000 を越えたもの。燃料の確認で
  止まった長いループはすぐ）を wasm の関数 1 つに組み、import `bjvm_jit.install(bytes, length)` でホストに渡す。
  ホストは b-jvm のメモリと関数表を import させて instantiate し、関数表に足して番号を返す
  （→ `src/jit-imports.ts`。動的リンクではメモリと関数表はローダーが作って `env` に渡す）。
  `BJVM_JIT=0` で切れる
- コンパイルしたコードは**インタプリタのフレームの上で動く**。ローカル変数とオペランドスタックの値は、命令の
  境目ではいつもインタプリタと同じスロットにある。だから扱わない命令（モニター・未解決の命令など）では pc を
  書いて戻るだけでインタプリタが続きを走らせ（`JIT_STATUS_BAIL`）、動く GC もインタプリタのフレームと同じに
  値を動かせる。例外も pc を書いて戻り、ドライバ（`interpret_2`）が普段どおり handler を探す
- 呼び出し: 呼び先がコンパイル済みなら、フレームを wasm の中で積んで直接呼ぶ（静的・special・単相のキャッシュ、
  vtable・itable の多相も）。getter と Kotlin の null 検査はその場で済ませる。それ以外は `jit_rt_invoke`
  （`interpreter2.c`）が intrinsic・同期 native・コンパイル済みの呼び先を受け持ち、残りはフレームを積んで
  ドライバに渡す。ホストのスタックを守るため、入れ子は `JIT_MAX_NESTING`（64）まで
- `interpreter2.c` の変更は、ドライバの `java_interpret_begin` で `jit_try_run` を呼ぶところと、燃料の確認の
  あとで `jit_note_hot_loop` を呼ぶところ、`jit_rt_invoke` だけ。intrinsic の enum は `jit.h` に移した

## wasm32-unknown-linux-musl への移行（2026-11 追加分）

BrowserOS の musl libc（Progate/libc、`wasm32-linux-musl` の multiarch sysroot）へ載せ替えた差分。
syscall は `wasi_snapshot_preview1` ではなく `browser_os_linux.syscall` へ出るので、libc 由来の
`__wasi_*` API と「wasi-libc だから」で書かれていた経路を普通の POSIX へ直している。
（このとき作った静的 reactor は、下の「`-pie` への移行」で動的実行ファイルへ変わっている）

- `cmake/WasiSupport.cmake` → `cmake/LinuxMuslSupport.cmake` … SjLj
  （`-mllvm -wasm-enable-sjlj`。`__wasm_setjmp`/`__wasm_longjmp` は musl の libc.a が持つので
  `-lsetjmp` は要らない）と `-mnontrapping-fptoint` は残し、WASI のエミュレーション系の
  定義と `-lwasi-emulated-mman` を落とした。リンクは `-static -nostartfiles` にして
  `-Wl,--entry=_initialize` を渡す（reactor の入口は sysroot に無いので、musl ソースの
  `crt1-reactor.c` と `init_libc_export.c` を `build-wasm.mjs` が `BJVM_LINUX_MUSL_CRT_OBJECTS` として
  組んで渡す）。`--export-dynamic`・`--export-table`・`--growable-table`・stack-size は変わらない
- `CMakeLists.txt` … `CMAKE_SYSTEM_NAME STREQUAL "WASI"` ではなく `Linux` + `wasm32` を見る
  （`BJVM_LINUX_MUSL`）。静的なので `CMAKE_POSITION_INDEPENDENT_CODE` と `bjvm` SHARED は対象外。
  musl の非 threads libc.a は弱い `pthread_create`/`pthread_join` を持つ（profiler.c は既定で
  コンパイルされない）ので `vm/wasi_pthread_stub.c` は消した
- `natives/share/sun/nio/ch/` … socket ABI（`BJVM_WASI_IMPORT`）と `__wasi_fd_*`・`poll_oneoff` を
  socket/bind/connect/accept4/readv/writev/recvmsg/sendto/getsockopt/setsockopt/shutdown/poll/
  fcntl の普通の POSIX に置き換え（`bjvm_net.h`）。JDK が渡す level/opt（SocketOptionRegistry）は
  Linux の実値そのままなので変換しない。ソケットは `SOCK_NONBLOCK` で開きっぱなしにし、
  ブロッキングの Java I/O は引き続き yield ループでエミュレートする（`bjvm_fd_set_nonblock` は
  帳簿だけ——実 fd の O_NONBLOCK を落とすとブロッキング syscall が VM を止める）
- `natives/share/sun/nio/fs/UnixNativeDispatcher.c` … wasi-libc の errno・oflags 変換表を消した
  （musl の値は Linux のものと一致し、同梱 JDK の UnixConstants も Linux 生成なので一致する）。
  `allocationGranularity0` は 4096 に戻す（BrowserOS の mmap(2) はページ単位の写しを返す）。
  `available0` の `ioctl(FIONREAD)` 経路を有効化
- `vm/bin/main.c` … 起動時の `PWD` への `chdir` を消した（cwd はカーネルがプロセスごとに持ち、
  open(2) が相対パスをそこから解く）
- `vm/bjvm.h`・`vm/jit.c`・`natives/share/jdk/internal/util/SystemProps.c` … WASI の判定の
  マクロを `defined(__wasm__) && !defined(EMSCRIPTEN)` に変えた（KEEPALIVE・JIT・
  `java.home=/jdk` は musl でも必要）
- `natives/share/java/io/posix-fs.c`・`natives/share/java/lang/System.c` … WASI の判定を
  落とし（musl は `__unix__`/`__linux__` を定義するのでそのまま POSIX 経路）
- `natives/share/sun/nio/ch/IOUtil.c` … `__wasi_random_get` を `getrandom(2)` に
- `vm/wasi_sbrk.c` … 消した。musl の malloc は brk/mmap を `browser_os_linux.syscall` へ打つ

## `-pie`（動的実行ファイル）への移行（2026-11 追加分）

musl を**静的に**組み込むのをやめ、`NEEDED libc.so` を宣言する `-pie` にした差分。ローダー
（browser-dyld）が libc.so を `/usr/lib/wasm32-linux-musl/libc.so` から読み、メモリ・関数表・
スタックを `env` に渡して instantiate するので、この wasm には libc のコードも crt も入らない。
ホストの駆動の形（初期化のあと `ffi_*` を呼ぶ）は変えていない——`_start` を 1 度走らせると
crt が `init_modules` で全モジュールのコンストラクタを回し、`main` はその時点で `_Exit` する
（`vm/bin/main.c`。`exit`/`return` だとデストラクタが VM の下を崩すので `_Exit`）。

- `cmake/LinuxMuslSupport.cmake` … `-static -nostartfiles -Wl,--entry=_initialize` と
  `BJVM_LINUX_MUSL_CRT_OBJECTS`（crt1-reactor.c・init_libc_export.c）を落とし、
  `-rdynamic`（`ffi_*` と実行時の関数を名前で export）だけ残した。`-pie`・Scrt1.o・`-lc` は
  ドライバの既定なので書かない。`--export-table`・`--growable-table`・stack-size も要らない
  ——メモリ・関数表・スタックはローダーが作る側で、表は最初から上限無しに伸びる
- `vm/bin/main.c` … 空だった `main` に `_Exit(0)` を書いた（上記）
- `vm/bin/main.c` … `main` は引数 2 つ（`envp` を取らない）。clang が wasm で `__main_argc_argv` と名付けるのは
  引数が 2 つの main だけで、crt1 はそれか `__main_void` しか呼ばない。3 引数だと main が呼ばれずに起動が止まる
- `cmake/LinuxMuslSupport.cmake` … 無い名前は Linux と同じくリンクのエラーにする。`--unresolved-symbols=import-dynamic`
  （何でも import にする）は使わない——無い関数がリンクのエラーにならず、実行時に初めて落ちる。ローダーが配る
  `__c_longjmp`（setjmp/longjmp のタグ）と `__fe_status`（FP 例外フラグ）は wasm-ld が自分で import にする
  （Progate/llvm-project#37・#39）。`--unwindlib=none` も要らない（C のリンクは libunwind を NEEDED にしない）
- （Progate/packages の）`packages/browser-java/src/os-executable.ts` … 直接 instantiate + `initializeWasiP1WebAssemblyInstance` をやめ、
  `BrowserDyld.loadProgram` で libc.so と一緒に起こし、`startWasiP1WebAssemblyInstance` で
  `_start` を回す。malloc/free は libc.so の export から、メモリと関数表はローダーの実物から取り、
  メインの export と畳んで JavaVm へ渡す

## システムの zlib（2026-10 追加分）

- `vm/CMakeLists.txt` … `BJVM_LINUX_MUSL` では FetchContent の zlib（zlibstatic を静的リンク）をやめ、
  `find_package(ZLIB)` で sysroot の libz.so を動的に結ぶ（`NEEDED libz.so`）。実行時の libz.so は
  @progate/browser-libc の `libcPackage()` が置く。ネイティブ版の分岐は upstream のまま

## upstream に追従するとき

上記の変更はいずれも既存コードへの小さな条件分岐で、`vm/interpreter2.c` 以外は独立している。
upstream を取り込み直すときは、このブランチを upstream の新しいコミットへ rebase し、`<新しいコミット>+progate` の
ブランチとして push する（Progate/packages の browser-java の `BJVM_SOURCE` を上げる）。
`interpreter2.c` の `#if BJVM_WASM` は upstream が `#ifdef EMSCRIPTEN` に戻していないか要確認。
