package com.cosmosex.floppy

import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.X509TrustManager

/** Server not reachable on any of its addresses. */
class UnreachableException(cause: Throwable?) : IOException(cause)

/** The server doesn't take our token: reset on the server, or paired with another one. */
class UnpairedException : IOException()

/** The server answered with an error, message is what it said. */
class ServerException(message: String) : IOException(message)

data class ApiDevice(
    val mac: String,
    val name: String,
    val connected: Boolean,
    val image: String,
    val imageName: String,
    val dir: String,
)

data class DirEntry(val name: String, val path: String)

data class ImageList(
    val dir: String,
    val parent: String,
    val dirs: List<DirEntry>,
    val images: List<String>,
)

/**
 * Trusts only the certificate chains which lead to the CA of the paired server. Its fingerprint comes from the QR code, so
 * the CA doesn't have to be installed on the phone, and the token never goes to anything else.
 */
class PinnedCaTrustManager(private val fingerprint: String) : X509TrustManager {
    override fun checkServerTrusted(chain: Array<out X509Certificate>, authType: String?) {
        val caIndex = chain.indexOfFirst { sha256Hex(it.encoded) == fingerprint }
        if (caIndex < 0) {
            throw CertificateException("CA of the paired server is not in the chain")
        }
        try {
            chain[0].checkValidity()
            for (i in 0 until caIndex) {
                chain[i].verify(chain[i + 1].publicKey)
            }
        } catch (e: Exception) {
            throw CertificateException("invalid chain", e)
        }
    }

    override fun checkClientTrusted(chain: Array<out X509Certificate>, authType: String?) {
        throw CertificateException("not used")
    }

    override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()

    companion object {
        fun sha256Hex(data: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }
    }
}

/** Client of /app/v1 of the webserver. Blocking, so call it on Worker. */
class Api(private val pairing: Pairing, private val store: Store?) {
    private val socketFactory: SSLSocketFactory = SSLContext.getInstance("TLS").run {
        init(null, arrayOf(PinnedCaTrustManager(pairing.fingerprint)), null)
        socketFactory
    }

    fun devices(): List<ApiDevice> {
        val array = JSONObject(request("GET", "/app/v1/state", null)).getJSONArray("devices")
        return List(array.length()) { i ->
            val d = array.getJSONObject(i)
            ApiDevice(
                mac = d.getString("mac"),
                name = d.getString("name"),
                connected = d.optBoolean("connected"),
                image = d.optString("image"),
                imageName = d.optString("imageName"),
                dir = d.optString("dir"),
            )
        }
    }

    /** Images and sub folders of the folder, without dir of the folder the device browses. */
    fun images(mac: String, dir: String?): ImageList {
        var path = "/app/v1/images?mac=" + encode(mac)
        if (!dir.isNullOrEmpty()) {
            path += "&dir=" + encode(dir)
        }
        val o = JSONObject(request("GET", path, null))
        val dirs = o.getJSONArray("dirs")
        val images = o.getJSONArray("images")
        return ImageList(
            dir = o.getString("dir"),
            parent = o.optString("parent"),
            dirs = List(dirs.length()) { i ->
                val e = dirs.getJSONObject(i)
                DirEntry(e.getString("name"), e.getString("path"))
            },
            images = List(images.length()) { i -> images.getString(i) },
        )
    }

    /** Returns the full path of the inserted image. */
    fun insert(mac: String, dir: String, name: String): String {
        val body = JSONObject().put("mac", mac).put("dir", dir).put("name", name).toString()
        return JSONObject(request("POST", "/app/v1/insert", body)).getString("image")
    }

    /** Tries the addresses from the QR code, starting with the one which worked the last time. */
    private fun request(method: String, path: String, body: String?): String {
        val preferred = store?.preferredAddress
        val addresses = pairing.addresses.sortedBy { if (it == preferred) 0 else 1 }
        var lastError: IOException? = null
        for (address in addresses) {
            try {
                val result = requestOnce(address, method, path, body)
                if (address != preferred) {
                    store?.preferredAddress = address
                }
                return result
            } catch (e: UnpairedException) {
                throw e
            } catch (e: ServerException) {
                throw e
            } catch (e: IOException) { // includes failed certificate check - this isn't our server, try the next one
                lastError = e
            }
        }
        throw UnreachableException(lastError)
    }

    private fun requestOnce(address: String, method: String, path: String, body: String?): String {
        val host = if (address.contains(':')) "[$address]" else address
        val conn = URL("https://$host:${pairing.port}$path").openConnection() as HttpsURLConnection
        conn.sslSocketFactory = socketFactory
        conn.setHostnameVerifier { _, _ -> true } // the check of the CA above is what matters, the IP can change
        conn.connectTimeout = CONNECT_TIMEOUT_MS
        conn.readTimeout = READ_TIMEOUT_MS
        conn.instanceFollowRedirects = false
        conn.requestMethod = method
        conn.setRequestProperty("Authorization", "Bearer ${pairing.token}")
        try {
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.outputStream.use { it.write(body.toByteArray()) }
            }
            val code = conn.responseCode
            if (code == HttpURLConnection.HTTP_UNAUTHORIZED) {
                throw UnpairedException()
            }
            if (code !in 200..299) {
                val text = conn.errorStream?.use { it.readBytes().decodeToString() }?.trim()
                throw ServerException(if (text.isNullOrEmpty()) "HTTP $code" else text)
            }
            return conn.inputStream.use { it.readBytes().decodeToString() }
        } finally {
            conn.disconnect()
        }
    }

    private fun encode(s: String) = URLEncoder.encode(s, "UTF-8")

    private companion object {
        const val CONNECT_TIMEOUT_MS = 2500
        const val READ_TIMEOUT_MS = 5000
    }
}
