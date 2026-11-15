/*
 * DeviceIdentity.kt
 * Scopo: identità persistente del ricevitore, pubblicata nei record TXT mDNS
 * (_airplay._tcp e _raop._tcp) e usata nell'handshake pair-verify.
 *
 * Formato del file (lunghezza fissa, 83 byte; nessun parsing a lunghezza variabile):
 *   [0]      versione (1)
 *   [1..6]   MAC virtuale (unicast, "localmente amministrato")
 *   [7..22]  UUID dell'istanza (valore "pi")
 *   [23..34] IV AES-GCM (12 byte)
 *   [35..82] seed Ed25519 cifrato (32 byte) + tag GCM (16 byte)
 * I byte 0..22 sono legati al testo cifrato come AAD: se vengono modificati o
 * scambiati, la decifratura fallisce.
 *
 * Sicurezza:
 *  - La chiave AES-256 è nell'AndroidKeyStore, non esportabile.
 *  - Il file sta in noBackupFilesDir: escluso dai backup per costruzione.
 *  - Le operazioni sul Keystore possono durare decine di ms: NON chiamare dal main thread.
 */
package com.irplay.tv.core

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import android.util.Log
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.nio.ByteBuffer
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.SecureRandom
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class DeviceIdentity private constructor(
    private val mac: ByteArray,
    private val uuid: UUID,
    private val pk: ByteArray,
    private val file: AtomicFile,
) {
    /** Valore TXT "deviceid": "AA:BB:CC:DD:EE:FF". */
    val deviceId: String get() = mac.joinToString(":") { "%02X".format(it) }

    /** Prefisso del nome del servizio _raop._tcp ("AABBCCDDEEFF@iRPlay"). */
    val raopPrefix: String get() = mac.joinToString("") { "%02X".format(it) }

    /** Valore TXT "pi". */
    val pairingId: String get() = uuid.toString()

    /** Valore TXT "pk": chiave pubblica Ed25519 in esadecimale minuscolo. */
    val publicKeyHex: String get() = pk.joinToString("") { "%02x".format(it) }

    /** Chiave pubblica Ed25519 (32 byte). Restituisce una copia: l'array interno non va esposto. */
    val publicKey: ByteArray get() = pk.copyOf()

    /**
     * Decifra e restituisce il seed Ed25519, per l'handshake.
     * Il chiamante DEVE azzerarlo appena finito: `seed.fill(0)`.
     */
    @Throws(IOException::class, GeneralSecurityException::class)
    fun loadSeed(): ByteArray = decryptSeed(readRecord(file) ?: throw IOException("identità assente"))

    companion object {
        private const val TAG = "iRPlay_Identity"
        private const val FILE_NAME = "identity.bin"
        private const val KEY_ALIAS = "irplay_identity_wrap_v1"
        private const val VERSION: Byte = 1
        private const val MAC_LEN = 6
        private const val HEADER_LEN = 1 + MAC_LEN + 16
        private const val IV_LEN = 12
        private const val SEED_LEN = 32
        private const val TAG_BITS = 128
        private const val CT_LEN = SEED_LEN + TAG_BITS / 8
        private const val RECORD_LEN = HEADER_LEN + IV_LEN + CT_LEN

        private val random = SecureRandom()

        /** Carica l'identità o, se manca o è illeggibile, ne crea una nuova. Da chiamare fuori dal main thread. */
        @Synchronized
        fun loadOrCreate(context: Context): DeviceIdentity {
            val file = AtomicFile(File(context.noBackupFilesDir, FILE_NAME))
            try {
                readRecord(file)?.let { return fromRecord(it, file) }
            } catch (e: GeneralSecurityException) {
                // Caso tipico: chiave del Keystore persa o file manomesso. L'iPhone dovrà
                // solo ripetere l'abbinamento: non è un errore da mostrare all'utente.
                Log.w(TAG, "Identità non decifrabile, ne creo una nuova")
            } catch (e: IOException) {
                Log.w(TAG, "Identità non leggibile, ne creo una nuova")
            }
            return create(file)
        }

        private fun create(file: AtomicFile): DeviceIdentity {
            val mac = ByteArray(MAC_LEN).also { random.nextBytes(it) }
            // bit0 = 0 (unicast), bit1 = 1 (localmente amministrato): non collide con MAC reali.
            mac[0] = ((mac[0].toInt() and 0xFC) or 0x02).toByte()
            val uuid = UUID.randomUUID()
            val header = ByteBuffer.allocate(HEADER_LEN)
                .put(VERSION).put(mac)
                .putLong(uuid.mostSignificantBits).putLong(uuid.leastSignificantBits)
                .array()

            val seed = ByteArray(SEED_LEN).also { random.nextBytes(it) }
            try {
                val cipher = Cipher.getInstance("AES/GCM/NoPadding")
                cipher.init(Cipher.ENCRYPT_MODE, wrapKey())   // IV casuale generato dal Keystore
                cipher.updateAAD(header)
                val ct = cipher.doFinal(seed)
                val iv = cipher.iv
                check(iv.size == IV_LEN && ct.size == CT_LEN)

                val record = header + iv + ct
                val out = file.startWrite()
                try {
                    out.write(record)
                    file.finishWrite(out)          // scrittura atomica: nessun file a metà
                } catch (e: IOException) {
                    file.failWrite(out)
                    throw e
                }
                val pk = NativeBridge.ed25519PublicKey(seed)
                    ?: throw GeneralSecurityException("derivazione pk fallita")
                return DeviceIdentity(mac, uuid, pk, file)
            } finally {
                seed.fill(0)
            }
        }

        private fun fromRecord(record: ByteArray, file: AtomicFile): DeviceIdentity {
            val seed = decryptSeed(record)
            try {
                val pk = NativeBridge.ed25519PublicKey(seed)
                    ?: throw GeneralSecurityException("derivazione pk fallita")
                val bb = ByteBuffer.wrap(record, 1 + MAC_LEN, 16)
                val uuid = UUID(bb.long, bb.long)
                return DeviceIdentity(record.copyOfRange(1, 1 + MAC_LEN), uuid, pk, file)
            } finally {
                seed.fill(0)
            }
        }

        /** Restituisce null se il file non esiste; lancia IOException se la lunghezza o la versione non sono valide. */
        private fun readRecord(file: AtomicFile): ByteArray? {
            val bytes = try { file.readFully() } catch (e: FileNotFoundException) { return null }
            if (bytes.size != RECORD_LEN || bytes[0] != VERSION) throw IOException("formato non valido")
            return bytes
        }

        private fun decryptSeed(record: ByteArray): ByteArray {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(
                Cipher.DECRYPT_MODE, wrapKey(),
                GCMParameterSpec(TAG_BITS, record, HEADER_LEN, IV_LEN)
            )
            cipher.updateAAD(record, 0, HEADER_LEN)
            val seed = cipher.doFinal(record, HEADER_LEN + IV_LEN, CT_LEN)
            if (seed.size != SEED_LEN) { seed.fill(0); throw GeneralSecurityException("seed non valido") }
            return seed
        }

        private fun wrapKey(): SecretKey {
            val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            (ks.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
            return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
                init(
                    KeyGenParameterSpec.Builder(
                        KEY_ALIAS,
                        KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
                    )
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setKeySize(256)
                        .build()
                )
            }.generateKey()
        }
    }
}