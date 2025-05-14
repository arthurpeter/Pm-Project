package com.example.cameraapp

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Bundle
import android.provider.Settings
import android.webkit.WebChromeClient
import android.webkit.WebResourceError
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import com.example.cameraapp.ui.theme.CameraAppTheme
import kotlinx.coroutines.*
import java.net.HttpURLConnection
import java.net.URL

class MainActivity : ComponentActivity() {
    private var webView: WebView? = null

    @SuppressLint("UnusedMaterial3ScaffoldPaddingParameter")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            CameraAppTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) {
                    WebViewContainer { webView = it }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()

        CoroutineScope(Dispatchers.IO).launch {
            // Try for up to 6 seconds
            val maxAttempts = 3
            var reachable = false

            repeat(maxAttempts) {
                reachable = isCameraReachable()
                if (reachable) return@repeat
                delay(500)
            }

            withContext(Dispatchers.Main) {
                if (reachable) {
                    webView?.loadUrl("http://192.168.4.1")
                } else {
                    promptToConnect(this@MainActivity)
                }
            }
        }
    }
}

@SuppressLint("SetJavaScriptEnabled")
@Composable
fun WebViewContainer(setWebView: (WebView) -> Unit) {
    val context = LocalContext.current

    AndroidView(
        factory = { ctx ->
            WebView(ctx).apply {
                settings.javaScriptEnabled = true
                settings.domStorageEnabled = true
                settings.useWideViewPort = true
                settings.loadWithOverviewMode = true
                settings.allowFileAccess = true
                settings.mediaPlaybackRequiresUserGesture = false

                webViewClient = object : WebViewClient() {
                    override fun onReceivedError(
                        view: WebView?,
                        errorCode: Int,
                        description: String?,
                        failingUrl: String?
                    ) {
                        super.onReceivedError(view, errorCode, description, failingUrl)
                        promptToConnect(context)
                    }
                }
                webChromeClient = WebChromeClient()

                setWebView(this) // Save reference to WebView for later use
            }
        },
        modifier = Modifier.fillMaxSize()
    )
}

suspend fun isCameraReachable(): Boolean = withContext(Dispatchers.IO) {
    return@withContext try {
        val connection = URL("http://192.168.4.1").openConnection() as HttpURLConnection
        connection.connectTimeout = 2000
        connection.readTimeout = 2000
        connection.requestMethod = "GET"
        connection.connect()
        val responseCode = connection.responseCode
        responseCode in 200..299
    } catch (e: Exception) {
        false
    }
}

fun promptToConnect(context: Context) {
    Toast.makeText(
        context,
        "Please connect to the ESP32 camera WiFi.",
        Toast.LENGTH_LONG
    ).show()

    context.startActivity(Intent(Settings.ACTION_WIFI_SETTINGS).apply {
        flags = Intent.FLAG_ACTIVITY_NEW_TASK
    })
}
