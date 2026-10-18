package com.icast.tv

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.widget.TextView
import com.icast.tv.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.sampleText.text = stringFromJNI()
        // TEST Fase 1C (da rimuovere): il Keystore non va usato dal main thread.
        Thread {
            val id = com.icast.tv.core.DeviceIdentity.loadOrCreate(applicationContext)
            val info = "deviceid ${id.deviceId}\npi ${id.pairingId}\npk ${id.publicKeyHex.take(16)}…"
            runOnUiThread { binding.sampleText.text = info }
        }.start()
    }

    /**
     * A native method that is implemented by the 'tv' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String

    companion object {
        init {
            System.loadLibrary("icast_native") // deve coincidere con project() in CMakeLists.txt
        }
    }
}