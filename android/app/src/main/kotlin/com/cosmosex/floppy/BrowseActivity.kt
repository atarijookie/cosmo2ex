package com.cosmosex.floppy

import android.app.Activity
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast

/** Images on the server. Choosing one selects it, the same as going to it with prev / next - Insert is then a tap away. */
class BrowseActivity : Activity() {
    private class Row(val label: String, val onClick: () -> Unit) {
        override fun toString() = label
    }

    private lateinit var floppy: Floppy
    private lateinit var title: TextView
    private lateinit var list: ListView
    private lateinit var mac: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        floppy = Floppy(this)
        mac = intent.getStringExtra(EXTRA_MAC) ?: run { finish(); return }

        val padding = (16 * resources.displayMetrics.density).toInt()
        title = TextView(this).apply {
            setPadding(padding, padding, padding, padding)
            setTextAppearance(android.R.style.TextAppearance_Material_Subhead)
        }
        list = ListView(this)
        setContentView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                fitsSystemWindows = true
                addView(title)
                addView(list, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
            },
        )
        list.setOnItemClickListener { parent, _, position, _ ->
            (parent.getItemAtPosition(position) as Row).onClick()
        }

        load(floppy.store.selection(mac)?.dir, opening = true)
    }

    private fun load(dir: String?, opening: Boolean = false) {
        Worker.run({
            val pairing = floppy.store.pairing ?: throw UnpairedException()
            Api(pairing, floppy.store).images(mac, dir)
        }) { result ->
            result.onSuccess { show(it) }.onFailure {
                if (opening && dir != null && it is ServerException) {
                    load(null, opening = true) // the selected folder is gone - start where the server says
                } else if (opening) {
                    title.text = Floppy.errorText(this, it)
                } else { // e.g. no rights to this folder - stay in the one that's shown
                    Toast.makeText(this, Floppy.errorText(this, it), Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun show(content: ImageList) {
        title.text = content.dir
        val rows = ArrayList<Row>()
        if (content.parent.isNotEmpty()) {
            rows += Row(getString(R.string.up)) { load(content.parent) }
        }
        content.dirs.forEach { d -> rows += Row("📁 " + d.name) { load(d.path) } }
        content.images.forEach { name ->
            rows += Row(name) {
                floppy.store.setSelection(mac, Selection(content.dir, name))
                FloppyWidget.updateAll(this)
                finish()
            }
        }
        list.adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, rows)
    }

    companion object {
        const val EXTRA_MAC = "mac"
    }
}
