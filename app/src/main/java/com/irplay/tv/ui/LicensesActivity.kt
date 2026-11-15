/*
 * LicensesActivity.kt
 * Scopo: avvisi di copyright e testo della licenza, come richiesto dalla GPL-3 (§4-6)
 * per chi distribuisce l'APK, e dalle licenze delle librerie incluse (Apache-2.0 per
 * mbedTLS, ISC per libsodium, LGPL-2.1+ per le tabelle FairPlay di UxPlay).
 * I testi stanno in res/raw: niente WebView né rete.
 */
package com.irplay.tv.ui

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.irplay.tv.R
import com.irplay.tv.databinding.ActivityLicensesBinding

class LicensesActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val binding = ActivityLicensesBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.licenseText.text = buildString {
            append(readRaw(R.raw.third_party_notices))
            append("\n\n")
            append(readRaw(R.raw.gpl3))
        }
        binding.scroll.requestFocus()   // il D-pad scorre il testo
    }

    private fun readRaw(id: Int): String =
        resources.openRawResource(id).bufferedReader(Charsets.UTF_8).use { it.readText() }
}
