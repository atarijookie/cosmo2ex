package com.cosmosex.floppy

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.ImageButton
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity() {
    private lateinit var floppy: Floppy
    private lateinit var devicesSpinner: Spinner
    private lateinit var statusText: TextView

    private var devices: List<ApiDevice> = emptyList()
    private val handler = Handler(Looper.getMainLooper())
    private var resumed = false
    private var busy = false

    // changes made on the device or in the web UI show up while the app is open
    private val poll = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, POLL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        floppy = Floppy(this)
        devicesSpinner = findViewById(R.id.devices)
        statusText = findViewById(R.id.status)

        findViewById<ImageButton>(R.id.prev).setOnClickListener { step(-1) }
        findViewById<ImageButton>(R.id.next).setOnClickListener { step(+1) }
        findViewById<Button>(R.id.insert).setOnClickListener { insert() }
        findViewById<Button>(R.id.browse).setOnClickListener {
            activeDevice()?.let { d ->
                startActivity(Intent(this, BrowseActivity::class.java).putExtra(BrowseActivity.EXTRA_MAC, d.mac))
            }
        }

        devicesSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                val mac = devices.getOrNull(position)?.mac ?: return
                if (mac != floppy.store.activeMac) {
                    floppy.store.activeMac = mac
                    setStatus("") // it was about the previous device
                    showDevice()
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        handleIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        showPanels()
        devices = floppy.store.cachedDevices
        showDevices()
        handler.post(poll)
    }

    override fun onPause() {
        super.onPause()
        resumed = false
        handler.removeCallbacks(poll)
    }

    private fun handleIntent(intent: Intent?) {
        intent?.getStringExtra(FloppyWidget.EXTRA_MAC)?.let { floppy.store.activeMac = it } // tap on the widget
        val uri = intent?.data ?: return
        setIntent(Intent()) // don't pair again after the screen was rotated

        val pairing = Pairing.parse(uri)
        if (pairing == null) {
            Toast.makeText(this, R.string.pair_invalid, Toast.LENGTH_LONG).show()
            return
        }

        val current = floppy.store.pairing
        if (current != null && current.fingerprint != pairing.fingerprint) {
            // a link can come from any web page, so a working pairing isn't replaced without asking
            AlertDialog.Builder(this)
                .setTitle(R.string.pair_replace_title)
                .setMessage(getString(R.string.pair_replace_text, pairing.addresses.first()))
                .setPositiveButton(R.string.replace) { _, _ -> pair(pairing) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        } else {
            pair(pairing)
        }
    }

    /** The new settings are stored only after they have worked. */
    private fun pair(pairing: Pairing) {
        setStatus(getString(R.string.pair_connecting))
        Worker.run({
            Api(pairing, null).devices()
        }) { result ->
            result.onSuccess {
                val current = floppy.store.pairing
                if (current != null && current.fingerprint != pairing.fingerprint) {
                    floppy.store.forgetDevices()
                }
                floppy.store.pairing = pairing
                setStatus(getString(R.string.pair_ok))
                showPanels()
                refresh(keepStatus = true)
            }.onFailure {
                setStatus(getString(R.string.pair_failed))
            }
        }
    }

    private fun showPanels() {
        val paired = floppy.store.pairing != null
        findViewById<View>(R.id.not_paired).visibility = if (paired) View.GONE else View.VISIBLE
        findViewById<View>(R.id.controls).visibility = if (paired) View.VISIBLE else View.GONE
    }

    private fun refresh(keepStatus: Boolean = false) {
        if (floppy.store.pairing == null || busy) {
            return
        }
        Worker.run({ floppy.refresh() }) { result ->
            if (!resumed || busy) return@run
            result.onSuccess {
                devices = it
                showDevices()
                if (!keepStatus && statusIsError) {
                    setStatus("")
                }
                FloppyWidget.updateAll(this)
            }.onFailure { showError(it) }
        }
    }

    private fun showDevices() {
        val names = devices.map { d ->
            if (d.connected) d.name else getString(R.string.offline_suffix, d.name)
        }
        if (names != shownNames) {
            shownNames = names
            devicesSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, names)
        }
        val index = devices.indexOfFirst { it.mac == floppy.store.activeMac }
        if (index >= 0) {
            devicesSpinner.setSelection(index)
        } else if (devices.isNotEmpty()) {
            floppy.store.activeMac = devices[0].mac
        }
        showDevice()
    }

    private var shownNames: List<String> = emptyList()
    private var statusIsError = false
    private var insertedName: String? = null // shown as "Inserted", until something else gets in the drive

    private fun activeDevice(): ApiDevice? =
        devices.firstOrNull { it.mac == floppy.store.activeMac } ?: devices.firstOrNull()

    private fun showDevice() {
        val imageName = findViewById<TextView>(R.id.image_name)
        val driveStatus = findViewById<TextView>(R.id.drive_status)
        val device = activeDevice()
        if (device == null) {
            imageName.setText(R.string.no_devices)
            driveStatus.text = ""
            return
        }
        val view = floppy.view(device)
        if (insertedName != null && insertedName != view.inDriveName) {
            setStatus("")
        }
        imageName.text = view.selectedName.ifEmpty { getString(R.string.no_image) }
        driveStatus.text = when {
            view.selectedIsInDrive -> getString(R.string.in_drive)
            view.inDriveName.isEmpty() -> getString(R.string.drive_empty)
            else -> getString(R.string.in_drive_other, view.inDriveName)
        }
    }

    private fun step(delta: Int) {
        val device = activeDevice() ?: return
        busy = true
        Worker.run({ floppy.step(device.mac, delta) }) { result -> finishAction(result) }
    }

    private fun insert() {
        val device = activeDevice() ?: return
        busy = true
        Worker.run({ floppy.insert(device.mac) }) { result ->
            finishAction(result)
            result.onSuccess {
                setStatus(getString(R.string.inserted, it))
                insertedName = it
            }
        }
    }

    private fun finishAction(result: Result<*>) {
        busy = false
        result.onFailure { showError(it) }
        result.onSuccess { setStatus("") }
        devices = floppy.store.cachedDevices
        showDevices()
        FloppyWidget.updateAll(this)
    }

    private fun setStatus(text: String) {
        statusIsError = false
        insertedName = null
        statusText.text = text
    }

    private fun showError(error: Throwable) {
        statusIsError = true
        statusText.text = Floppy.errorText(this, error)
    }

    private companion object {
        const val POLL_MS = 3000L
    }
}
