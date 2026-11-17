@file:Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
package dev.jollymaam.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.webkit.RenderProcessGoneDetail
import android.util.Log
import android.webkit.MimeTypeMap
import android.os.Environment
import android.provider.DocumentsContract
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.ArrayAdapter
import android.widget.FrameLayout
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** The pad settings page (web/, packaged as assets/web) in a WebView, with
 * what the WebView lacks supplied by Bridge: USB, Bluetooth, the flasher. */
class MainActivity : Activity() {
    companion object {
        /** The page's origin: https, so a secure context (crypto.subtle). */
        const val HOST = "appassets.androidplatform.net"
        private const val FILE_CHOOSER = 1
        private const val PERMISSIONS = 2
        private var rendererGone = 0L
        private val TYPES = mapOf("html" to "text/html", "js" to "text/javascript", "mjs" to "text/javascript",
            "css" to "text/css", "json" to "application/json", "svg" to "image/svg+xml", "png" to "image/png",
            "zip" to "application/zip")
    }

    private lateinit var web: WebView
    private lateinit var bridge: Bridge
    private lateinit var frame: FrameLayout
    private var files: ValueCallback<Array<Uri>>? = null
    private val usbAsks = ConcurrentHashMap<String, CountDownLatch>()
    @Volatile private var permissionsAnswered: CountDownLatch? = null
    private val usbPermissionAction get() = "$packageName.USB_PERMISSION"
    private val usbManager by lazy { getSystemService(UsbManager::class.java) }

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
            when (intent.action) {
                usbPermissionAction -> usbAsks[device.deviceName]?.countDown()
                UsbManager.ACTION_USB_DEVICE_DETACHED -> bridge.detached(device)
            }
        }
    }

    /** The page's files from the APK; anything else is not ours to load. */
    private fun asset(uri: Uri): WebResourceResponse? {
        if (uri.host != HOST) return null
        val path = uri.path.orEmpty().trimStart('/').ifEmpty { "index.html" }
        val type = TYPES[path.substringAfterLast('.', "")] ?: "application/octet-stream"
        return try {
            if (path.split('/').any { it == ".." }) throw IOException()
            WebResourceResponse(type, "utf-8", assets.open("web/$path"))
        } catch (e: IOException) {
            WebResourceResponse("text/plain", "utf-8", 404, "Not Found", emptyMap(), "".byteInputStream())
        }
    }

    /** The bars' background and icons follow the page's light or dark
     * colours (web/style.css --bg). */
    private fun barColours() {
        val night = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES
        frame.setBackgroundColor(if (night) 0xFF161615.toInt() else 0xFFFAFAF9.toInt())
        if (Build.VERSION.SDK_INT >= 30) {
            val light = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            window.insetsController?.setSystemBarsAppearance(if (night) 0 else light, light)
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        barColours()
    }

    @SuppressLint("SetJavaScriptEnabled")   // the page is ours, from the APK
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WebView.setWebContentsDebuggingEnabled(BuildConfig.DEBUG)   // chrome://inspect
        web = WebView(this)
        web.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            allowContentAccess = false
        }
        web.webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest) = asset(request.url)
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                if (request.url.host == HOST) return false
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, request.url)) }   // links out: the browser
                return true
            }
            // The page's renderer died (out of memory, a WebView bug): the
            // page again, not the whole app gone; twice in 10 s, the app ends.
            override fun onRenderProcessGone(view: WebView, detail: RenderProcessGoneDetail): Boolean {
                val now = System.currentTimeMillis()
                if (now - rendererGone < 10000) finish() else recreate()
                rendererGone = now
                return true
            }
        }
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(view: WebView, callback: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
                files?.onReceiveValue(null)
                files = callback
                return try {
                    // The page's accept list as MIME types the picker knows
                    // (".zip" is not one, and zips go by several), else any.
                    // OPEN_DOCUMENT with the phone's storage shown, starting
                    // in Download/JollyMaam: the picker's Downloads view
                    // leaves out files MediaStore does not list.
                    val types = params.acceptTypes.flatMap { mimeTypes(it) }.distinct()
                    Log.i("JollyMaam", "file chooser: accept ${params.acceptTypes.toList()} -> types $types")
                    val pick = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                        addCategory(Intent.CATEGORY_OPENABLE)
                        type = "*/*"
                        if (types.isNotEmpty()) putExtra(Intent.EXTRA_MIME_TYPES, types.toTypedArray())
                        putExtra("android.content.extra.SHOW_ADVANCED", true)
                    }
                    startActivityForResult(pick, FILE_CHOOSER)
                    true
                } catch (e: Exception) {
                    files = null
                    false
                }
            }
        }
        bridge = Bridge(this, web)
        web.addJavascriptInterface(bridge, "JollyMaamAndroid")
        // The page below the status bar, above the navigation bar and the
        // keyboard: Android 15 draws apps edge to edge (targetSdk 35), so
        // from Android 11 on we do too and pad by the bars' insets.
        frame = FrameLayout(this)
        frame.addView(web)
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.statusBarColor = Color.TRANSPARENT
            window.navigationBarColor = Color.TRANSPARENT
            frame.setOnApplyWindowInsetsListener { v, insets ->
                val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout() or WindowInsets.Type.ime())
                v.setPadding(i.left, i.top, i.right, i.bottom)
                WindowInsets.CONSUMED
            }
        }
        barColours()
        setContentView(frame)
        val filter = IntentFilter(usbPermissionAction).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED) else registerReceiver(receiver, filter)
        if (savedInstanceState != null) web.restoreState(savedInstanceState)
        else web.loadUrl("https://$HOST/index.html")
    }

    /** An accept entry (".zip", "application/zip") as MIME types; a zip
     * also as the other types it is saved under. */
    private fun mimeTypes(accept: String): List<String> {
        val t = accept.trim().lowercase()
        val mime = if (t.startsWith(".")) MimeTypeMap.getSingleton().getMimeTypeFromExtension(t.drop(1)) else t.ifEmpty { null }
        return when (mime) {
            null -> emptyList()
            "application/zip" -> listOf(mime, "application/x-zip-compressed", "application/x-zip")
            else -> listOf(mime)
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != FILE_CHOOSER) return
        files?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data))
        files = null
    }

    /** Back: the page's own back first (a screen back to its home). */
    override fun onBackPressed() {
        if (web.canGoBack()) web.goBack() else super.onBackPressed()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, results: IntArray) {
        if (requestCode == PERMISSIONS) permissionsAnswered?.countDown()
    }

    override fun onDestroy() {
        permissionsAnswered?.countDown()
        usbAsks.values.forEach { it.countDown() }
        bridge.close()
        unregisterReceiver(receiver)
        web.destroy()
        super.onDestroy()
    }

    // ---- for the bridge, from its worker threads: they wait for the answer ----

    private fun ui(block: () -> Unit) = runOnUiThread { if (!isFinishing && !isDestroyed) block() }

    /** USB access to the device, asked for if need be. */
    fun usbPermission(device: UsbDevice): Boolean {
        if (usbManager.hasPermission(device)) return true
        val latch = CountDownLatch(1)
        usbAsks[device.deviceName] = latch
        ui {
            usbManager.requestPermission(device, PendingIntent.getBroadcast(this, device.deviceId,
                Intent(usbPermissionAction).setPackage(packageName), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE))
        }
        return try { latch.await(60, TimeUnit.SECONDS) && usbManager.hasPermission(device) }
        finally { usbAsks.remove(device.deviceName) }
    }

    private val bluetoothPermissions get() = if (Build.VERSION.SDK_INT >= 31)
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)   // scanning, before Android 12

    private fun granted() = bluetoothPermissions.all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    /** Scanning and connecting, asked for if need be. */
    fun bluetoothPermission(): Boolean {
        if (granted()) return true
        val latch = CountDownLatch(1)
        permissionsAnswered = latch
        ui { requestPermissions(bluetoothPermissions, PERMISSIONS) }
        latch.await(120, TimeUnit.SECONDS)
        return granted()
    }

    /** One of the items, or null. */
    fun pick(title: String, items: List<String>): Int? {
        val chooser = liveChooser(title)
        ui { items.forEach(chooser::add) }
        return chooser.await()
    }

    /** A list to choose from that fills while it shows (a Bluetooth scan). */
    inner class LiveChooser(title: String) {
        private val latch = CountDownLatch(1)
        @Volatile private var chosen: Int? = null
        private lateinit var adapter: ArrayAdapter<String>
        init {
            ui {
                adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_list_item_1)
                AlertDialog.Builder(this@MainActivity).setTitle(title)
                    .setAdapter(adapter) { _, i -> chosen = i }
                    .setNegativeButton("Cancel", null)
                    .setOnDismissListener { latch.countDown() }
                    .show()
            }
        }
        /** On the UI thread. */
        fun add(item: String) { adapter.add(item) }
        fun await(): Int? {
            while (!latch.await(1, TimeUnit.SECONDS)) if (isDestroyed) return null
            return chosen
        }
    }

    fun liveChooser(title: String) = LiveChooser(title)
}
