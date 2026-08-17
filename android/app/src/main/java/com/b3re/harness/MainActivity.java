package com.b3re.harness;

import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Burnout 3: Takedown RE harness -- Android entry point.
 *
 * The native harness reads every asset through a relative "build/..." path
 * (see the repo-root Makefile / src/burnout3_full.c).  Rather than rewrite
 * those paths, we unpack assets/burnout3_assets.zip into the app's internal
 * storage once, and the native bootstrap (b3_android.c) chdir()s there, so
 * "build/tracks/US_C3_V1/..." resolves exactly as it does on the desktop.
 *
 * The zip carries a stamp file (build/ASSET_STAMP) written by
 * android/pack_assets.sh; when the stamp in the APK differs from the one on
 * disk, the tree is re-extracted.  That is what makes an extractor rerun
 * ("regenerate build/tracks/...", then repack) flow into the installed app.
 */
public class MainActivity extends SDLActivity {

    private static final String TAG = "Burnout3";
    private static final String ZIP = "burnout3_assets.zip";
    private static final String STAMP = "build/ASSET_STAMP";

    /** libSDL2.so then libmain.so; libSDL2_image.so rides in as a DT_NEEDED. */
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Must happen before SDLActivity spins up the native thread.
        try {
            extractAssetsIfNeeded();
        } catch (IOException e) {
            Log.e(TAG, "asset extraction failed", e);
        }
        super.onCreate(savedInstanceState);
    }

    private void extractAssetsIfNeeded() throws IOException {
        File root = getFilesDir();
        String want = readStampFromApk();
        File have = new File(root, STAMP);

        if (want != null && have.isFile() && want.equals(readFile(have))) {
            Log.i(TAG, "assets up to date (" + want.trim() + ")");
            return;
        }

        Log.i(TAG, "extracting " + ZIP + " into " + root.getAbsolutePath());
        long t0 = System.currentTimeMillis();
        long bytes = 0;
        byte[] buf = new byte[256 * 1024];

        InputStream raw = getAssets().open(ZIP, android.content.res.AssetManager.ACCESS_STREAMING);
        ZipInputStream zin = new ZipInputStream(raw);
        try {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                File out = safeChild(root, e.getName());
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                OutputStream os = new FileOutputStream(out);
                try {
                    int n;
                    while ((n = zin.read(buf)) > 0) { os.write(buf, 0, n); bytes += n; }
                } finally {
                    os.close();
                }
            }
        } finally {
            zin.close();
        }
        Log.i(TAG, "extracted " + (bytes / (1024 * 1024)) + " MiB in "
                + (System.currentTimeMillis() - t0) + " ms");
    }

    /** Reject zip entries that would escape the destination directory. */
    private static File safeChild(File root, String name) throws IOException {
        File f = new File(root, name);
        String rootPath = root.getCanonicalPath() + File.separator;
        if (!f.getCanonicalPath().startsWith(rootPath)) {
            throw new IOException("zip entry escapes destination: " + name);
        }
        return f;
    }

    private String readStampFromApk() {
        InputStream raw = null;
        ZipInputStream zin = null;
        try {
            raw = getAssets().open(ZIP, android.content.res.AssetManager.ACCESS_STREAMING);
            zin = new ZipInputStream(raw);
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (STAMP.equals(e.getName())) {
                    return slurp(zin);
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "no stamp in " + ZIP, e);
        } finally {
            try { if (zin != null) zin.close(); else if (raw != null) raw.close(); }
            catch (IOException ignored) { }
        }
        return null;
    }

    private static String readFile(File f) throws IOException {
        InputStream in = new java.io.FileInputStream(f);
        try { return slurp(in); } finally { in.close(); }
    }

    private static String slurp(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        byte[] b = new byte[4096];
        int n;
        while ((n = in.read(b)) > 0) bos.write(b, 0, n);
        return new String(bos.toByteArray(), "UTF-8");
    }
}
