package com.cosmosex.floppy

import android.content.Context
import android.net.Uri
import org.json.JSONArray
import org.json.JSONObject

/** What the QR code holds: cosmosex://pair?a=ip1,ip2&p=port&t=token&f=fingerprint of the CA */
data class Pairing(
    val addresses: List<String>,
    val port: Int,
    val token: String,
    val fingerprint: String,
) {
    companion object {
        fun parse(uri: Uri): Pairing? {
            if (uri.scheme != "cosmosex" || uri.host != "pair") {
                return null
            }
            val addresses = uri.getQueryParameter("a")?.split(',')?.filter { it.isNotBlank() }.orEmpty()
            val port = uri.getQueryParameter("p")?.toIntOrNull() ?: return null
            val token = uri.getQueryParameter("t").orEmpty()
            val fingerprint = uri.getQueryParameter("f").orEmpty().lowercase()
            val validFingerprint = fingerprint.length == 64 && fingerprint.all { it in "0123456789abcdef" }
            if (addresses.isEmpty() || port !in 1..65535 || token.isEmpty() || !validFingerprint) {
                return null
            }
            return Pairing(addresses, port, token, fingerprint)
        }
    }
}

/** The image the user moved to with prev / next, which isn't necessarily the one in the drive - like on the device. */
data class Selection(val dir: String, val name: String)

class Store(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences("ce_floppy", Context.MODE_PRIVATE)

    var pairing: Pairing?
        get() {
            val token = prefs.getString("token", null) ?: return null
            return Pairing(
                addresses = prefs.getString("addresses", "").orEmpty().split(',').filter { it.isNotEmpty() },
                port = prefs.getInt("port", 0),
                token = token,
                fingerprint = prefs.getString("fingerprint", "").orEmpty(),
            )
        }
        set(p) {
            if (p == null) return
            prefs.edit()
                .putString("addresses", p.addresses.joinToString(","))
                .putInt("port", p.port)
                .putString("token", p.token)
                .putString("fingerprint", p.fingerprint)
                .remove("preferredAddress")
                .apply()
        }

    var preferredAddress: String?
        get() = prefs.getString("preferredAddress", null)
        set(a) = prefs.edit().putString("preferredAddress", a).apply()

    /** Device chosen in the app. */
    var activeMac: String?
        get() = prefs.getString("activeMac", null)
        set(mac) = prefs.edit().putString("activeMac", mac).apply()

    fun widgetMac(widgetId: Int): String? = prefs.getString("widgetMac$widgetId", null)

    fun setWidgetMac(widgetId: Int, mac: String) = prefs.edit().putString("widgetMac$widgetId", mac).apply()

    fun forgetWidget(widgetId: Int) = prefs.edit().remove("widgetMac$widgetId").apply()

    fun selection(mac: String): Selection? {
        val name = prefs.getString("selName$mac", null) ?: return null
        return Selection(prefs.getString("selDir$mac", "").orEmpty(), name)
    }

    fun setSelection(mac: String, selection: Selection?) {
        val edit = prefs.edit()
        if (selection == null) {
            edit.remove("selName$mac").remove("selDir$mac")
        } else {
            edit.putString("selName$mac", selection.name).putString("selDir$mac", selection.dir)
        }
        edit.apply()
    }

    /** The image in the drive when we looked the last time, a change means someone else has inserted something. */
    fun lastImage(mac: String): String? = prefs.getString("lastImage$mac", null)

    fun setLastImage(mac: String, image: String) = prefs.edit().putString("lastImage$mac", image).apply()

    /** Last known devices, so the widget can show something without asking the server. */
    var cachedDevices: List<ApiDevice>
        get() {
            val array = JSONArray(prefs.getString("devices", "[]"))
            return List(array.length()) { i ->
                val d = array.getJSONObject(i)
                ApiDevice(
                    d.getString("mac"),
                    d.getString("name"),
                    d.getBoolean("connected"),
                    d.getString("image"),
                    d.getString("imageName"),
                    d.getString("dir"),
                )
            }
        }
        set(devices) {
            val array = JSONArray()
            devices.forEach { d ->
                array.put(
                    JSONObject()
                        .put("mac", d.mac)
                        .put("name", d.name)
                        .put("connected", d.connected)
                        .put("image", d.image)
                        .put("imageName", d.imageName)
                        .put("dir", d.dir),
                )
            }
            prefs.edit().putString("devices", array.toString()).apply()
        }

    /** After pairing with a different server nothing we know about the devices is valid. */
    fun forgetDevices() {
        val edit = prefs.edit().remove("devices").remove("activeMac")
        prefs.all.keys
            .filter { it.startsWith("selName") || it.startsWith("selDir") || it.startsWith("lastImage") }
            .forEach { edit.remove(it) }
        edit.apply()
    }
}
