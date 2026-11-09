package com.cosmosex.floppy

import android.app.Activity
import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import android.widget.ListView
import android.widget.RemoteViews
import android.widget.TextView

class FloppyWidget : AppWidgetProvider() {
    /** Called by Android about every 30 minutes, and when a widget is added. */
    override fun onUpdate(context: Context, manager: AppWidgetManager, widgetIds: IntArray) {
        val appContext = context.applicationContext
        render(appContext, widgetIds, null) // right away from what we know
        val pending = goAsync()
        Worker.run {
            val error = runCatching { Floppy(appContext).refresh() }.exceptionOrNull()
            updateAll(appContext, error?.let { Floppy.errorText(appContext, it, short = true) })
            pending.finish()
        }
    }

    override fun onDeleted(context: Context, widgetIds: IntArray) {
        val store = Store(context)
        widgetIds.forEach { store.forgetWidget(it) }
    }

    companion object {
        const val EXTRA_MAC = "mac"
        const val ACTION_PREV = "com.cosmosex.floppy.PREV"
        const val ACTION_NEXT = "com.cosmosex.floppy.NEXT"
        const val ACTION_INSERT = "com.cosmosex.floppy.INSERT"

        fun updateAll(context: Context, status: String? = null) {
            val manager = AppWidgetManager.getInstance(context)
            val ids = manager.getAppWidgetIds(ComponentName(context, FloppyWidget::class.java))
            render(context, ids, status)
        }

        /** The device of the widget; a widget without a known one shows the first device. */
        fun widgetDevice(store: Store, widgetId: Int): ApiDevice? {
            val devices = store.cachedDevices
            val mac = store.widgetMac(widgetId)
            return devices.firstOrNull { it.mac == mac } ?: devices.firstOrNull()
        }

        private fun render(context: Context, widgetIds: IntArray, status: String?) {
            val manager = AppWidgetManager.getInstance(context)
            val floppy = Floppy(context)
            for (id in widgetIds) {
                val views = RemoteViews(context.packageName, R.layout.widget)
                val device = widgetDevice(floppy.store, id)
                if (device == null) {
                    views.setTextViewText(R.id.widget_device, context.getString(R.string.app_name))
                    val paired = floppy.store.pairing != null
                    views.setTextViewText(
                        R.id.widget_image,
                        context.getString(if (paired) R.string.no_devices else R.string.not_paired_title),
                    )
                    views.setTextViewText(R.id.widget_status, status.orEmpty())
                } else {
                    val view = floppy.view(device)
                    views.setTextViewText(
                        R.id.widget_device,
                        if (device.connected) device.name else context.getString(R.string.offline_suffix, device.name),
                    )
                    views.setTextViewText(
                        R.id.widget_image,
                        view.selectedName.ifEmpty { context.getString(R.string.no_image) },
                    )
                    views.setTextViewText(
                        R.id.widget_status,
                        status ?: when {
                            view.selectedIsInDrive -> context.getString(R.string.in_drive)
                            view.inDriveName.isEmpty() -> context.getString(R.string.drive_empty)
                            else -> context.getString(R.string.in_drive_other, view.inDriveName)
                        },
                    )
                }
                views.setOnClickPendingIntent(R.id.widget_prev, action(context, ACTION_PREV, id))
                views.setOnClickPendingIntent(R.id.widget_next, action(context, ACTION_NEXT, id))
                views.setOnClickPendingIntent(R.id.widget_insert, action(context, ACTION_INSERT, id))

                val open = Intent(context, MainActivity::class.java)
                device?.let { open.putExtra(EXTRA_MAC, it.mac) }
                views.setOnClickPendingIntent(
                    R.id.widget_image,
                    PendingIntent.getActivity(
                        context, id, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
                    ),
                )
                manager.updateAppWidget(id, views)
            }
        }

        private fun action(context: Context, action: String, widgetId: Int): PendingIntent {
            val intent = Intent(context, WidgetActionReceiver::class.java)
                .setAction(action)
                .putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, widgetId)
            // the widget ID in the request code, so buttons of several widgets don't share one PendingIntent
            return PendingIntent.getBroadcast(
                context, widgetId, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        }
    }
}

class WidgetActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val appContext = context.applicationContext
        val widgetId = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID)
        val action = intent.action
        val pending = goAsync()
        Worker.run {
            val floppy = Floppy(appContext)
            val status = runCatching {
                // no background updates, so what's on the widget can be old - and it can be the first tap after pairing
                val device = FloppyWidget.widgetDevice(floppy.store, widgetId)
                    ?: run { floppy.refresh(); FloppyWidget.widgetDevice(floppy.store, widgetId) }
                    ?: throw ServerException(appContext.getString(R.string.no_devices))
                when (action) {
                    FloppyWidget.ACTION_PREV -> floppy.step(device.mac, -1)
                    FloppyWidget.ACTION_NEXT -> floppy.step(device.mac, +1)
                    FloppyWidget.ACTION_INSERT -> floppy.insert(device.mac)
                }
            }.exceptionOrNull()?.let { Floppy.errorText(appContext, it, short = true) }
            FloppyWidget.updateAll(appContext, status)
            pending.finish()
        }
    }
}

/** Shown when a widget is added: which device it's for. With one device there's nothing to ask. */
class WidgetConfigActivity : Activity() {
    private var widgetId = AppWidgetManager.INVALID_APPWIDGET_ID

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setResult(RESULT_CANCELED)
        widgetId = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID)
        if (widgetId == AppWidgetManager.INVALID_APPWIDGET_ID) {
            finish()
            return
        }

        val floppy = Floppy(this)
        Worker.run({ runCatching { floppy.refresh() }.getOrElse { floppy.store.cachedDevices } }) { result ->
            val devices = result.getOrDefault(emptyList())
            if (devices.size <= 1) {
                done(floppy, devices.firstOrNull()) // also not paired yet - the widget tells that
            } else {
                choose(floppy, devices)
            }
        }
    }

    private fun choose(floppy: Floppy, devices: List<ApiDevice>) {
        val padding = (16 * resources.displayMetrics.density).toInt()
        val title = TextView(this).apply {
            setText(R.string.choose_device)
            setPadding(padding, padding, padding, padding)
            setTextAppearance(android.R.style.TextAppearance_Material_Subhead)
        }
        val list = ListView(this)
        list.addHeaderView(title, null, false)
        list.fitsSystemWindows = true
        list.adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, devices.map { it.name })
        list.setOnItemClickListener { _, _: View, position, _ ->
            done(floppy, devices[position - list.headerViewsCount])
        }
        setContentView(list)
    }

    private fun done(floppy: Floppy, device: ApiDevice?) {
        device?.let { floppy.store.setWidgetMac(widgetId, it.mac) }
        FloppyWidget.updateAll(this)
        setResult(RESULT_OK, Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, widgetId))
        finish()
    }
}
