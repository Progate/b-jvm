package bjvm;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.MalformedURLException;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;

/**
 * b-jvm の「システムクラスローダ」相当。
 *
 * クラスの読み込み自体は bootstrap に任せたまま、アプリケーションのクラスパス
 * から読まれたクラスの getClassLoader() がこのローダを返す。getResource(s) 系は
 * VM がリソースを実ファイルとして見せる仕組み（findResources0 ネイティブが
 * 実パスの一覧を返す）に載せているので、META-INF/spring.factories のような
 * jar 内リソースも通常の file: URL として読める。
 */
public final class SystemClassLoader extends ClassLoader {
  /**
   * 親は本物の AppClassLoader と同じく platform ローダーにする。ServiceLoader は
   * ローダーから親をたどって各ローダーの service 表を見るので、親が null だと
   * platform ローダー所属のモジュール（jdk.zipfs の ZipFileSystemProvider など）の
   * provider が見つからない（kotlinc が klib を開く FileSystems.newFileSystem が落ちる）。
   * クラスの解決は下の loadClass が bootstrap に渡すので、親は使わない
   */
  public SystemClassLoader() {
    super(ClassLoader.getPlatformClassLoader());
  }

  private static native String[] findResources0(String name);

  /**
   * クラス解決は全エントリをスキャンする bootstrap 検索に委譲する。
   * ClassLoader 既定の loadClass（findBootstrapClassOrNull + findClass）だと
   * findClass が必ず投げるので、VM の forName0（loader=null → bootstrap 検索）
   * に渡す方が確実。java.lang.reflect.Proxy のインタフェース可視性チェック
   * （forName(name, false, loader)）もこの経路を通る。
   */
  @Override
  protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
    // このローダ配下に動的定義されたクラス（CGLIB など）を先に見る
    Class<?> c = findLoadedClass(name);
    if (c != null) {
      return c;
    }
    return Class.forName(name, resolve, null);
  }

  @Override
  public Enumeration<URL> getResources(String name) throws IOException {
    List<URL> urls = new ArrayList<>();
    for (String path : findResources0(name)) {
      urls.add(new File(path).toURI().toURL());
    }
    return Collections.enumeration(urls);
  }

  @Override
  public URL getResource(String name) {
    String[] paths = findResources0(name);
    if (paths.length == 0) {
      return null;
    }
    try {
      return new File(paths[0]).toURI().toURL();
    } catch (MalformedURLException e) {
      return null;
    }
  }

  @Override
  public InputStream getResourceAsStream(String name) {
    try {
      URL url = getResource(name);
      return url == null ? null : url.openStream();
    } catch (IOException e) {
      return null;
    }
  }
}
