package com.cosmosex.floppy

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executors

/** All server requests run here, one by one, so quick taps on prev / next are handled in the order they came. */
object Worker {
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    fun run(work: () -> Unit) {
        executor.execute(work)
    }

    fun <T> run(work: () -> T, done: (Result<T>) -> Unit) {
        executor.execute {
            val result = runCatching(work)
            main.post { done(result) }
        }
    }
}

/** What the app and the widget show for a device. */
data class DeviceView(
    val mac: String,
    val name: String,
    val connected: Boolean,
    val selectedName: String,   // shown as the big name, "" if there's nothing selected
    val inDriveName: String,    // "" if the drive is empty
    val selectedIsInDrive: Boolean,
)

/**
 * prev / next only move a selection stored on the phone, and Insert sends it to the server - the same as the buttons on the
 * device, so going through a list doesn't load every image on the way. Blocking functions, so call them on Worker.
 */
class Floppy(context: Context) {
    val store = Store(context)

    private fun api(): Api {
        val pairing = store.pairing ?: throw UnpairedException()
        return Api(pairing, store)
    }

    /** Asks the server for the devices. When an image in a drive changed elsewhere, the selection follows it. */
    fun refresh(): List<ApiDevice> {
        val devices = api().devices()
        for (d in devices) {
            if (d.image != store.lastImage(d.mac)) {
                store.setLastImage(d.mac, d.image)
                store.setSelection(d.mac, if (d.image.isEmpty()) null else Selection(d.dir, d.imageName))
            }
        }
        store.cachedDevices = devices
        return devices
    }

    /** Moves the selection by delta in the folder of the selected image. Wraps around, like on the device. */
    fun step(mac: String, delta: Int) {
        refresh()
        val api = api()
        val selection = store.selection(mac)
        val list = try {
            api.images(mac, selection?.dir)
        } catch (e: ServerException) { // the folder is gone or not allowed anymore - let the server choose one
            if (selection == null) throw e
            api.images(mac, null)
        }
        if (list.images.isEmpty()) {
            throw ServerException(NO_IMAGES)
        }
        val index = if (selection?.dir == list.dir) list.images.indexOf(selection.name) else -1
        val newIndex = when {
            index >= 0 -> (index + delta + list.images.size) % list.images.size
            delta > 0 -> 0
            else -> list.images.size - 1
        }
        store.setSelection(mac, Selection(list.dir, list.images[newIndex]))
    }

    /** Inserts the selected image, and returns its name. */
    fun insert(mac: String): String {
        val selection = store.selection(mac) ?: throw ServerException(NOTHING_SELECTED)
        val image = try {
            api().insert(mac, selection.dir, selection.name)
        } finally {
            runCatching { refresh() } // 502 from the server means the image is stored, so show the truth in any case
        }
        store.setLastImage(mac, image)
        return selection.name
    }

    fun view(device: ApiDevice): DeviceView {
        val selection = store.selection(device.mac)
        val selectedName = selection?.name ?: device.imageName
        val inDrive = device.image.isNotEmpty() &&
            (selection == null || (selection.dir == device.dir && selection.name == device.imageName))
        return DeviceView(device.mac, device.name, device.connected, selectedName, device.imageName, inDrive)
    }

    companion object {
        const val NO_IMAGES = "no images"
        const val NOTHING_SELECTED = "nothing selected"

        /** Message for the user, short: for the one line on the widget. */
        fun errorText(context: Context, error: Throwable, short: Boolean = false): String = when (error) {
            is UnpairedException -> context.getString(if (short) R.string.err_unpaired_short else R.string.err_unpaired)
            is UnreachableException -> context.getString(if (short) R.string.err_unreachable_short else R.string.err_unreachable)
            is ServerException -> when (error.message) {
                NO_IMAGES -> context.getString(R.string.err_no_images)
                NOTHING_SELECTED -> context.getString(R.string.err_nothing_selected)
                else -> error.message.orEmpty()
            }
            else -> error.message ?: error.toString()
        }
    }
}
