package com.ternak.tt.dpc

import android.app.admin.DeviceAdminReceiver
import android.content.Context
import android.content.Intent
import android.content.ComponentName
import android.app.admin.DevicePolicyManager
import android.os.UserManager
import android.util.Log

class TtDeviceAdminReceiver : DeviceAdminReceiver() {
    override fun onProfileProvisioningComplete(context: Context, intent: Intent) {
        val dpm = context.getSystemService(Context.DEVICE_POLICY_SERVICE) as DevicePolicyManager
        val componentName = ComponentName(context, TtDeviceAdminReceiver::class.java)

        try {
            dpm.setProfileName(componentName, "TernakTT")
            dpm.setProfileEnabled(componentName)

            // cross-profile intent config (optional, maybe helpful for launching from user 0)
            val intentFilter = android.content.IntentFilter(Intent.ACTION_MAIN)
            intentFilter.addCategory(Intent.CATEGORY_LAUNCHER)
            dpm.addCrossProfileIntentFilter(
                componentName,
                intentFilter,
                DevicePolicyManager.FLAG_MANAGED_CAN_ACCESS_PARENT or DevicePolicyManager.FLAG_PARENT_CAN_ACCESS_MANAGED
            )

            Log.i("TernakTTDPC", "Profile provisioning complete and enabled")
        } catch (e: Exception) {
            Log.e("TernakTTDPC", "Error during profile provisioning", e)
        }
    }
}
