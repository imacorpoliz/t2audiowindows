# T2AudioPort - WASAPI tone renderer.
#
# Opens the T2 endpoint in shared mode, starts the stream and continuously
# writes a 440 Hz sine wave for a few seconds so the system-buffer -> BAR1
# mirror (T2AudioStreamCopyTick) has real samples to deliver to the hardware.
# Listen for the tone to confirm end-to-end playback.
#
# Stage E (first audible run, GEMINI.md): pass -EndpointVolume 0.02..0.25.
# The probe then requires an explicit -DeviceId (no fallback to another
# endpoint), refuses an endpoint volume above 0.25 or an amplitude above 0.3,
# un-mutes the T2 endpoint at that volume only after reading it back, and
# mutes it again with volume 0 when the tone ends (also on errors). Without
# -EndpointVolume the endpoint volume and mute are left as they are.
#
# -SweepTo F sweeps logarithmically from -Frequency to F over -Seconds.
# Every tone fades in and out over 20 ms and the buffer is drained before
# Stop, so a tone never starts or ends with a click.
#
# -Channel N plays the tone on channel N only (the others get silence). On
# the stereo endpoint 0 = left, 1 = right. On the raw 6-channel endpoint
# (-RawChannels in the agent) 0/1 and 3/4 are the woofers and 2/5 the
# tweeters, which have NO protection filter there: the probe refuses a tone
# below 1 kHz on channels 2 and 5 of a 6-channel format. -1 = all channels.
#
# All COM work is done inside C# so the RCW-to-interface casts happen in the
# CLR (PowerShell cannot cast a raw COM object to a [ComImport] interface).

param(
    [string]$DeviceId = '',
    [double]$Seconds = 4.0,
    [double]$Frequency = 440.0,
    [double]$Amplitude = 0.25,
    [double]$EndpointVolume = -1.0,
    [int]$Channel = -1,
    [double]$SweepTo = 0.0
)

$ErrorActionPreference = 'Stop'

$code = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace T2WasapiTone {
public enum EDataFlow { eRender, eCapture, eAll }
public enum ERole { eConsole, eMultimedia, eCommunications }
[Flags] public enum CLSCTX : uint { INPROC_SERVER=1, INPROC_HANDLER=2, LOCAL_SERVER=4, REMOTE_SERVER=0x10, ALL=0x17 }

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
    [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out IMMDeviceCollection devices);
    [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
    [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
    [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
}
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceCollection {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int Item(uint index, out IMMDevice device);
}
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, CLSCTX clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    [PreserveSig] int OpenPropertyStore(uint access, out IntPtr props);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int GetState(out uint state);
}
[Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioClient {
    [PreserveSig] int Initialize(int shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, IntPtr audioSessionGuid);
    [PreserveSig] int GetBufferSize(out uint numFrames);
    [PreserveSig] int GetStreamLatency(out long latency);
    [PreserveSig] int GetCurrentPadding(out uint padding);
    [PreserveSig] int IsFormatSupported(int shareMode, IntPtr pFormat, out IntPtr closestMatch);
    [PreserveSig] int GetMixFormat(out IntPtr ppDeviceFormat);
    [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minPeriod);
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    [PreserveSig] int Reset();
    [PreserveSig] int SetEventHandle(IntPtr eventHandle);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
}
[Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioEndpointVolume {
    [PreserveSig] int RegisterControlChangeNotify(IntPtr notify);
    [PreserveSig] int UnregisterControlChangeNotify(IntPtr notify);
    [PreserveSig] int GetChannelCount(out uint count);
    [PreserveSig] int SetMasterVolumeLevel(float levelDb, ref Guid context);
    [PreserveSig] int SetMasterVolumeLevelScalar(float level, ref Guid context);
    [PreserveSig] int GetMasterVolumeLevel(out float levelDb);
    [PreserveSig] int GetMasterVolumeLevelScalar(out float level);
    [PreserveSig] int SetChannelVolumeLevel(uint channel, float levelDb, ref Guid context);
    [PreserveSig] int SetChannelVolumeLevelScalar(uint channel, float level, ref Guid context);
    [PreserveSig] int GetChannelVolumeLevel(uint channel, out float levelDb);
    [PreserveSig] int GetChannelVolumeLevelScalar(uint channel, out float level);
    [PreserveSig] int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, ref Guid context);
    [PreserveSig] int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
}
[Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint numFramesRequested, out IntPtr ppData);
    [PreserveSig] int ReleaseBuffer(uint numFramesWritten, uint dwFlags);
}

public static class Tone {
    static string H(int hr) { return "0x" + ((uint)hr).ToString("X8"); }

    // Stage E limits (GEMINI.md). Raised after the 2026-10-28 run, where
    // 0.03 x 0.05 x the driver's -18 dB (about -80 dB) was inaudible.
    public const double MaxEndpointVolume = 0.25;
    public const double MaxLowVolumeAmplitude = 0.3;

    static IAudioEndpointVolume EndpointVolume(IMMDevice device, StringBuilder sb) {
        Guid iid = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");
        object o;
        int hr = device.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o);
        sb.AppendLine("Activate(IAudioEndpointVolume) hr=" + H(hr));
        return (hr == 0 && o != null) ? (IAudioEndpointVolume)o : null;
    }

    // Set volume first, then un-mute, then read both back.
    static bool SetLowVolume(IAudioEndpointVolume vol, float level, StringBuilder sb) {
        Guid ctx = Guid.Empty;
        int hr = vol.SetMasterVolumeLevelScalar(level, ref ctx);
        sb.AppendLine("SetMasterVolumeLevelScalar(" + level + ") hr=" + H(hr));
        if (hr != 0) return false;
        float back;
        if (vol.GetMasterVolumeLevelScalar(out back) != 0 || back > (float)MaxEndpointVolume + 0.001f) {
            sb.AppendLine("volume read back " + back + " - refusing to un-mute");
            return false;
        }
        hr = vol.SetMute(false, ref ctx);
        sb.AppendLine("SetMute(false) hr=" + H(hr));
        bool muted;
        int mhr = vol.GetMute(out muted);
        int lhr = vol.GetMasterVolumeLevelScalar(out back);
        sb.AppendLine("endpoint mute=" + muted + " volume=" + back + " (hr " + H(mhr) + "/" + H(lhr) + ")");
        return hr == 0 && mhr == 0 && lhr == 0 && !muted && back <= (float)MaxEndpointVolume + 0.001f;
    }

    static void Silence(IAudioEndpointVolume vol, StringBuilder sb) {
        Guid ctx = Guid.Empty;
        vol.SetMute(true, ref ctx);
        vol.SetMasterVolumeLevelScalar(0.0f, ref ctx);
        bool muted; float level;
        vol.GetMute(out muted);
        vol.GetMasterVolumeLevelScalar(out level);
        sb.AppendLine("after run: endpoint mute=" + muted + " volume=" + level);
    }

    // Raw 6-channel ring: channels 2 and 5 are the unprotected tweeters.
    public const double MinTweeterFrequency = 1000.0;

    public static string Run(string targetId, double seconds, double frequency, double amplitude, double endpointVolume, int channel, double sweepTo) {
        var sb = new StringBuilder();
        bool lowVolume = endpointVolume >= 0.0;
        if (lowVolume) {
            if (string.IsNullOrEmpty(targetId)) { sb.AppendLine("LOW-VOLUME RUN REFUSED: -DeviceId is required"); return sb.ToString(); }
            if (endpointVolume > MaxEndpointVolume) { sb.AppendLine("LOW-VOLUME RUN REFUSED: endpoint volume " + endpointVolume + " > " + MaxEndpointVolume); return sb.ToString(); }
            if (amplitude > MaxLowVolumeAmplitude) { sb.AppendLine("LOW-VOLUME RUN REFUSED: amplitude " + amplitude + " > " + MaxLowVolumeAmplitude); return sb.ToString(); }
        }
        var enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));

        IMMDevice device = null;
        if (!string.IsNullOrEmpty(targetId)) {
            int ghr = enumerator.GetDevice(targetId, out device);
            sb.AppendLine("GetDevice hr=" + H(ghr));
            if (ghr != 0) device = null;
        }
        if (device == null && lowVolume) { sb.AppendLine("TARGET NOT FOUND - low-volume run refused"); return sb.ToString(); }
        if (device == null) {
            IMMDeviceCollection col;
            enumerator.EnumAudioEndpoints(EDataFlow.eRender, 1, out col);
            uint count; col.GetCount(out count);
            for (uint i = 0; i < count; i++) {
                IMMDevice d; col.Item(i, out d);
                string id; d.GetId(out id);
                if (device == null) device = d;   // fall back to first active render
            }
        }
        if (device == null) { sb.AppendLine("no active render endpoint found"); return sb.ToString(); }
        string devId; device.GetId(out devId);
        sb.AppendLine("device = " + devId);

        IAudioEndpointVolume vol = null;
        if (lowVolume) {
            vol = EndpointVolume(device, sb);
            if (vol == null) { sb.AppendLine("LOW-VOLUME RUN REFUSED: no endpoint volume"); return sb.ToString(); }
            if (!SetLowVolume(vol, (float)endpointVolume, sb)) {
                Silence(vol, sb);
                sb.AppendLine("LOW VOLUME NOT CONFIRMED - endpoint muted again, no stream started.");
                return sb.ToString();
            }
            sb.AppendLine("T2 endpoint at low volume " + endpointVolume + ".");
        }
        try {
            Play(device, seconds, frequency, amplitude, channel, sweepTo, sb);
        } finally {
            if (vol != null) Silence(vol, sb);
        }
        return sb.ToString();
    }

    static void Play(IMMDevice device, double seconds, double frequency, double amplitude, int channel, double sweepTo, StringBuilder sb) {
        Guid iid = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        object o;
        int hr = device.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o);
        sb.AppendLine("Activate hr=" + H(hr));
        if (hr != 0 || o == null) return;
        var client = (IAudioClient)o;

        IntPtr pfmt;
        hr = client.GetMixFormat(out pfmt);
        if (hr != 0 || pfmt == IntPtr.Zero) { sb.AppendLine("GetMixFormat hr=" + H(hr)); return; }
        var fmt = (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(pfmt, typeof(WAVEFORMATEXTENSIBLE));
        int ch = fmt.Format.nChannels;
        int rate = (int)fmt.Format.nSamplesPerSec;
        bool isFloat = fmt.SubFormat == new Guid("00000003-0000-0010-8000-00aa00389b71") ||
                       fmt.Format.wFormatTag == 3;
        int bits = fmt.Format.wBitsPerSample;
        if (!isFloat && bits != 16 && bits != 32) { sb.AppendLine("unsupported PCM container: " + bits + " bits"); return; }
        sb.AppendLine("MixFormat: ch=" + ch + " rate=" + rate + " bits=" + fmt.Format.wBitsPerSample + " float=" + isFloat);
        if (channel >= ch) { sb.AppendLine("CHANNEL REFUSED: channel " + channel + " >= " + ch + " channels"); return; }
        double lowest = (sweepTo > 0.0 && sweepTo < frequency) ? sweepTo : frequency;
        if (ch == 6 && lowest < MinTweeterFrequency && (channel == 2 || channel == 5 || channel < 0)) {
            sb.AppendLine("TWEETER REFUSED: " + lowest + " Hz on a raw 6-channel tweeter (needs >= " + MinTweeterFrequency + " Hz)");
            return;
        }
        if (sweepTo > 0.0) sb.AppendLine("sweep " + frequency + " -> " + sweepTo + " Hz");
        sb.AppendLine("channel = " + (channel < 0 ? "all" : channel.ToString()));

        hr = client.Initialize(0, 0, 10000000, 0, pfmt, IntPtr.Zero);
        sb.AppendLine("Initialize hr=" + H(hr));
        if (hr != 0) return;

        uint frames;
        hr = client.GetBufferSize(out frames);
        sb.AppendLine("GetBufferSize hr=" + H(hr) + " frames=" + frames);

        Guid riid = new Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2");
        object ro;
        hr = client.GetService(ref riid, out ro);
        sb.AppendLine("GetService(IAudioRenderClient) hr=" + H(hr));
        if (hr != 0 || ro == null) return;
        var render = (IAudioRenderClient)ro;

        hr = client.Start();
        sb.AppendLine("Start hr=" + H(hr));
        if (hr != 0) return;

        // 20 ms raised-cosine fade in/out (an abrupt start or stop is heard
        // as a click, which is easy to mistake for a speaker fault), an
        // optional logarithmic sweep frequency -> sweepTo, and a drain of the
        // buffered audio before Stop so the end of the tone is not cut off.
        double phase = 0.0;
        int totalFrames = (int)(seconds * rate);
        int fade = Math.Min(rate / 50, totalFrames / 2);
        bool sweep = sweepTo > 0.0 && sweepTo != frequency;
        double logRatio = sweep ? Math.Log(sweepTo / frequency) : 0.0;
        int written = 0;
        int iterations = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        long budgetMs = (long)(seconds * 1000.0) + 3000;

        while (written < totalFrames && sw.ElapsedMilliseconds < budgetMs) {
            uint padding;
            if (client.GetCurrentPadding(out padding) != 0) break;
            uint avail = frames - padding;
            if (avail > (uint)(totalFrames - written)) avail = (uint)(totalFrames - written);
            if (avail == 0) { Thread.Sleep(2); continue; }

            IntPtr p;
            hr = render.GetBuffer(avail, out p);
            if (hr != 0) { sb.AppendLine("GetBuffer hr=" + H(hr)); break; }

            double[] v = new double[avail];
            for (int f = 0; f < avail; f++) {
                int n = written + f;
                double env = 1.0;
                if (n < fade) env = 0.5 - 0.5 * Math.Cos(Math.PI * n / fade);
                else if (n >= totalFrames - fade) env = 0.5 - 0.5 * Math.Cos(Math.PI * (totalFrames - 1 - n) / fade);
                double hz = sweep ? frequency * Math.Exp(logRatio * n / totalFrames) : frequency;
                v[f] = amplitude * env * Math.Sin(phase);
                phase += 2.0 * Math.PI * hz / rate;
                if (phase > 2.0 * Math.PI) phase -= 2.0 * Math.PI;
            }

            if (isFloat) {
                float[] buf = new float[avail * ch];
                for (int f = 0; f < avail; f++) {
                    float s = (float)v[f];
                    for (int c = 0; c < ch; c++) buf[f * ch + c] = (channel < 0 || c == channel) ? s : 0.0f;
                }
                Marshal.Copy(buf, 0, p, buf.Length);
            } else if (bits == 32) {
                // 32-bit container, MSB-aligned (24-in-32 included).
                int[] buf = new int[avail * ch];
                for (int f = 0; f < avail; f++) {
                    int s = (int)(v[f] * 2147483647.0);
                    for (int c = 0; c < ch; c++) buf[f * ch + c] = (channel < 0 || c == channel) ? s : 0;
                }
                Marshal.Copy(buf, 0, p, buf.Length);
            } else {
                short[] buf = new short[avail * ch];
                for (int f = 0; f < avail; f++) {
                    short s = (short)(v[f] * 32767.0);
                    for (int c = 0; c < ch; c++) buf[f * ch + c] = (channel < 0 || c == channel) ? s : (short)0;
                }
                Marshal.Copy(buf, 0, p, buf.Length);
            }

            render.ReleaseBuffer(avail, 0);
            written += (int)avail;
            iterations++;
            Thread.Sleep(2);
        }

        // Let the engine play what is still buffered (up to the buffer length).
        {
            var drain = System.Diagnostics.Stopwatch.StartNew();
            uint left;
            while (client.GetCurrentPadding(out left) == 0 && left > 0 && drain.ElapsedMilliseconds < 2000) {
                Thread.Sleep(5);
            }
        }

        sb.AppendLine("wrote frames=" + written + " iterations=" + iterations);
        hr = client.Stop();
        sb.AppendLine("Stop hr=" + H(hr));
    }
}
}
'@

if (-not ('T2WasapiTone.Tone' -as [type])) {
    Add-Type -TypeDefinition $code
}

[T2WasapiTone.Tone]::Run($DeviceId, $Seconds, $Frequency, $Amplitude, $EndpointVolume, $Channel, $SweepTo)
