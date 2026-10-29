# Record-WasapiMic.ps1 - Record a short WAV file from default capture device and report audio energy
param(
    [string]$OutFile = "C:\Users\othysa\Desktop\mic_test.wav",
    [int]$DurationSeconds = 3
)

$ErrorActionPreference = 'Stop'

$code = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace T2MicRecorder {
    public enum EDataFlow { eRender, eCapture, eAll }
    public enum ERole { eConsole, eMultimedia, eCommunications }
    [Flags] public enum CLSCTX : uint { ALL = 0x17 }

    [StructLayout(LayoutKind.Sequential, Pack=2)]
    public struct WAVEFORMATEX {
        public ushort wFormatTag; public ushort nChannels; public uint nSamplesPerSec;
        public uint nAvgBytesPerSec; public ushort nBlockAlign; public ushort wBitsPerSample; public ushort cbSize;
    }
    [StructLayout(LayoutKind.Sequential, Pack=2)]
    public struct WAVEFORMATEXTENSIBLE {
        public WAVEFORMATEX Format; public ushort wValidBitsPerSample; public uint dwChannelMask; public Guid SubFormat;
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] public class MMDeviceEnumerator { }

    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceEnumerator {
        [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out IntPtr devices);
        [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
        [PreserveSig] int GetDevice(string pwstrId, out IMMDevice device);
    }

    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDevice {
        [PreserveSig] int Activate(ref Guid id, CLSCTX clsCtx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    }

    [Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioClient {
        [PreserveSig] int Initialize(int shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, IntPtr audioSessionGuid);
        [PreserveSig] int GetBufferSize(out uint numBufferFrames);
        [PreserveSig] int GetStreamLatency(out long hnsLatency);
        [PreserveSig] int GetCurrentPadding(out uint numPaddingFrames);
        [PreserveSig] int IsFormatSupported(int shareMode, IntPtr pFormat, out IntPtr closestMatch);
        [PreserveSig] int GetMixFormat(out IntPtr deviceFormat);
        [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minPeriod);
        [PreserveSig] int Start();
        [PreserveSig] int Stop();
        [PreserveSig] int Reset();
        [PreserveSig] int SetEventHandle(IntPtr eventHandle);
        [PreserveSig] int GetService(ref Guid id, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    }

    [Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioCaptureClient {
        [PreserveSig] int GetBuffer(out IntPtr pData, out uint numFramesToRead, out uint dwFlags, out ulong u64DevicePosition, out ulong u64QPCPosition);
        [PreserveSig] int ReleaseBuffer(uint numFramesRead);
        [PreserveSig] int GetNextPacketSize(out uint numFramesInNextPacket);
    }

    public class Recorder {
        public static string RecordToWav(string outPath, int seconds, out double maxPeak, out double rms, out int frameCount) {
            maxPeak = 0;
            rms = 0;
            frameCount = 0;
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            IMMDevice dev;
            int hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.eCapture, ERole.eConsole, out dev);
            if (hr != 0 || dev == null) return "No default capture device, hr=0x" + hr.ToString("X8");

            Guid iidClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
            object oClient;
            hr = dev.Activate(ref iidClient, CLSCTX.ALL, IntPtr.Zero, out oClient);
            if (hr != 0 || oClient == null) return "Activate IAudioClient hr=0x" + hr.ToString("X8");

            var client = (IAudioClient)oClient;
            IntPtr pfmt;
            hr = client.GetMixFormat(out pfmt);
            if (hr != 0 || pfmt == IntPtr.Zero) return "GetMixFormat hr=0x" + hr.ToString("X8");

            var fmt = (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(pfmt, typeof(WAVEFORMATEXTENSIBLE));
            int channels = fmt.Format.nChannels;
            int sampleRate = (int)fmt.Format.nSamplesPerSec;

            hr = client.Initialize(0, 0, 10000000, 0, pfmt, IntPtr.Zero);
            if (hr != 0) return "Initialize hr=0x" + hr.ToString("X8");

            Guid iidCapture = new Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317");
            object oCapture;
            hr = client.GetService(ref iidCapture, out oCapture);
            if (hr != 0 || oCapture == null) return "GetService IAudioCaptureClient hr=0x" + hr.ToString("X8");
            var capture = (IAudioCaptureClient)oCapture;

            hr = client.Start();
            if (hr != 0) return "Start hr=0x" + hr.ToString("X8");

            using (var ms = new MemoryStream()) {
                using (var bw = new BinaryWriter(ms)) {
                    var end = DateTime.UtcNow.AddSeconds(seconds);
                    double sumSq = 0;
                    long totalSamples = 0;

                    while (DateTime.UtcNow < end) {
                        uint packetSize = 0;
                        hr = capture.GetNextPacketSize(out packetSize);
                        if (hr == 0 && packetSize > 0) {
                            IntPtr pData;
                            uint numFrames;
                            uint flags;
                            ulong devPos, qpcPos;
                            hr = capture.GetBuffer(out pData, out numFrames, out flags, out devPos, out qpcPos);
                            if (hr == 0 && numFrames > 0) {
                                frameCount += (int)numFrames;
                                int numFloats = (int)(numFrames * channels);
                                float[] buf = new float[numFloats];
                                Marshal.Copy(pData, buf, 0, numFloats);

                                for (int i = 0; i < numFloats; i++) {
                                    float val = buf[i];
                                    double a = Math.Abs(val);
                                    if (a > maxPeak) maxPeak = a;
                                    sumSq += val * val;
                                    totalSamples++;

                                    if (val > 1.0f) val = 1.0f;
                                    if (val < -1.0f) val = -1.0f;
                                    short pcm = (short)(val * 32767f);
                                    bw.Write(pcm);
                                }
                                capture.ReleaseBuffer(numFrames);
                            }
                        } else {
                            Thread.Sleep(5);
                        }
                    }
                    client.Stop();

                    if (totalSamples > 0) rms = Math.Sqrt(sumSq / totalSamples);

                    byte[] pcmData = ms.ToArray();
                    using (var fs = new FileStream(outPath, FileMode.Create)) {
                        using (var w = new BinaryWriter(fs)) {
                            w.Write(new char[] { 'R', 'I', 'F', 'F' });
                            w.Write(36 + pcmData.Length);
                            w.Write(new char[] { 'W', 'A', 'V', 'E' });
                            w.Write(new char[] { 'f', 'm', 't', ' ' });
                            w.Write(16);
                            w.Write((short)1); // PCM
                            w.Write((short)channels);
                            w.Write(sampleRate);
                            w.Write(sampleRate * channels * 2);
                            w.Write((short)(channels * 2));
                            w.Write((short)16);
                            w.Write(new char[] { 'd', 'a', 't', 'a' });
                            w.Write(pcmData.Length);
                            w.Write(pcmData);
                        }
                    }
                }
            }
            return "OK";
        }
    }
}
'@

if (-not ('T2MicRecorder.Recorder' -as [type])) {
    Add-Type -TypeDefinition $code
}

$peak = 0.0
$rms = 0.0
$frames = 0
$res = [T2MicRecorder.Recorder]::RecordToWav($OutFile, $DurationSeconds, [ref]$peak, [ref]$rms, [ref]$frames)

[PSCustomObject]@{
    Status       = $res
    File         = $OutFile
    Exists       = Test-Path $OutFile
    LengthBytes  = if (Test-Path $OutFile) { (Get-Item $OutFile).Length } else { 0 }
    Frames       = $frames
    Peak         = [math]::Round($peak, 5)
    RMS          = [math]::Round($rms, 5)
    RMS_dBFS     = if ($rms -gt 0) { [math]::Round(20 * [math]::Log10($rms), 2) } else { -999 }
}
