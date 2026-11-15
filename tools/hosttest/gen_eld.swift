// Codifica 5 s di tono a 440 Hz in AAC-ELD 44,1 kHz stereo, 480 campioni per frame (come iOS nel mirroring).
// Output: eld.bin ([u16 LE len][frame]*) e stampa l'AudioSpecificConfig (magic cookie).
import AVFoundation

let rate = 44100.0
let pcmFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: 2, interleaved: false)!
var asbd = AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatMPEG4AAC_ELD, mFormatFlags: 0,
                                       mBytesPerPacket: 0, mFramesPerPacket: 480, mBytesPerFrame: 0,
                                       mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
let eldFormat = AVAudioFormat(streamDescription: &asbd)!
let conv = AVAudioConverter(from: pcmFormat, to: eldFormat)!
conv.bitRate = 128000

let total = Int(rate * 5)
let pcm = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: AVAudioFrameCount(total))!
pcm.frameLength = AVAudioFrameCount(total)
for i in 0..<total {
    let v = Float(0.3 * sin(2.0 * Double.pi * 440.0 * Double(i) / rate))
    pcm.floatChannelData![0][i] = v
    pcm.floatChannelData![1][i] = v
}
var fed = false
var out = Data()
var frames = 0
while true {
    let cbuf = AVAudioCompressedBuffer(format: eldFormat, packetCapacity: 8, maximumPacketSize: conv.maximumOutputPacketSize)
    var err: NSError?
    let status = conv.convert(to: cbuf, error: &err) { _, st in
        if fed { st.pointee = .endOfStream; return nil }
        fed = true; st.pointee = .haveData; return pcm
    }
    for p in 0..<Int(cbuf.packetCount) {
        let d = cbuf.packetDescriptions![p]
        var len = UInt16(d.mDataByteSize).littleEndian
        out.append(Data(bytes: &len, count: 2))
        out.append(Data(bytes: cbuf.data.advanced(by: Int(d.mStartOffset)), count: Int(d.mDataByteSize)))
        frames += 1
    }
    if status == .endOfStream || status == .error { break }
}
if let cookie = conv.magicCookie { print("cookie:", cookie.map { String(format: "%02x", $0) }.joined()) }
try! out.write(to: URL(fileURLWithPath: "eld.bin"))
print("frame ELD: \(frames), byte: \(out.count)")
