# T2AudioPort - WASAPI tone renderer.
#
# Opens the T2 endpoint in shared mode, starts the stream and continuously
# writes a 440 Hz sine wave for a few seconds so the system-buffer -> BAR1
# mirror (T2AudioStreamCopyTick) has real samples to deliver to the hardware.
# Listen for the tone to confirm end-to-end playback.
#
# All COM work is done inside C# so the RCW-to-interface casts happen in the
# CLR (PowerShell cannot cast a raw COM object to a [ComImport] interface).

param(
    [string]$DeviceId = '',
    [double]$Seconds = 4.0,
    [double]$Frequency = 440.0,
    [double]$Amplitude = 0.25
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
[Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint numFramesRequested, out IntPtr ppData);
    [PreserveSig] int ReleaseBuffer(uint numFramesWritten, uint dwFlags);
}

public static class Tone {
    static string H(int hr) { return "0x" + ((uint)hr).ToString("X8"); }

    public static string Run(string targetId, double seconds, double frequency, double amplitude) {
        var sb = new StringBuilder();
        var enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(typeof(MMDeviceEnumerator));

        IMMDevice device = null;
        if (!string.IsNullOrEmpty(targetId)) {
            int ghr = enumerator.GetDevice(targetId, out device);
            sb.AppendLine("GetDevice hr=" + H(ghr));
            if (ghr != 0) device = null;
        }
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

        Guid iid = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        object o;
        int hr = device.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o);
        sb.AppendLine("Activate hr=" + H(hr));
        if (hr != 0 || o == null) return sb.ToString();
        var client = (IAudioClient)o;

        IntPtr pfmt;
        hr = client.GetMixFormat(out pfmt);
        if (hr != 0 || pfmt == IntPtr.Zero) { sb.AppendLine("GetMixFormat hr=" + H(hr)); return sb.ToString(); }
        var fmt = (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(pfmt, typeof(WAVEFORMATEXTENSIBLE));
        int ch = fmt.Format.nChannels;
        int rate = (int)fmt.Format.nSamplesPerSec;
        bool isFloat = fmt.SubFormat == new Guid("00000003-0000-0010-8000-00aa00389b71");
        sb.AppendLine("MixFormat: ch=" + ch + " rate=" + rate + " bits=" + fmt.Format.wBitsPerSample + " float=" + isFloat);

        hr = client.Initialize(0, 0, 10000000, 0, pfmt, IntPtr.Zero);
        sb.AppendLine("Initialize hr=" + H(hr));
        if (hr != 0) return sb.ToString();

        uint frames;
        hr = client.GetBufferSize(out frames);
        sb.AppendLine("GetBufferSize hr=" + H(hr) + " frames=" + frames);

        Guid riid = new Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2");
        object ro;
        hr = client.GetService(ref riid, out ro);
        sb.AppendLine("GetService(IAudioRenderClient) hr=" + H(hr));
        if (hr != 0 || ro == null) return sb.ToString();
        var render = (IAudioRenderClient)ro;

        hr = client.Start();
        sb.AppendLine("Start hr=" + H(hr));
        if (hr != 0) return sb.ToString();

        double phase = 0.0;
        double step = 2.0 * Math.PI * frequency / rate;
        int totalFrames = (int)(seconds * rate);
        int written = 0;
        int iterations = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        long budgetMs = (long)(seconds * 1000.0) + 2000;

        while (written < totalFrames && sw.ElapsedMilliseconds < budgetMs) {
            uint padding;
            if (client.GetCurrentPadding(out padding) != 0) break;
            uint avail = frames - padding;
            if (avail == 0) { Thread.Sleep(2); continue; }

            IntPtr p;
            hr = render.GetBuffer(avail, out p);
            if (hr != 0) { sb.AppendLine("GetBuffer hr=" + H(hr)); break; }

            if (isFloat) {
                float[] buf = new float[avail * ch];
                for (int f = 0; f < avail; f++) {
                    float s = (float)(amplitude * Math.Sin(phase));
                    phase += step; if (phase > 2.0 * Math.PI) phase -= 2.0 * Math.PI;
                    for (int c = 0; c < ch; c++) buf[f * ch + c] = s;
                }
                Marshal.Copy(buf, 0, p, buf.Length);
            } else {
                short[] buf = new short[avail * ch];
                for (int f = 0; f < avail; f++) {
                    short s = (short)(amplitude * 32767.0 * Math.Sin(phase));
                    phase += step; if (phase > 2.0 * Math.PI) phase -= 2.0 * Math.PI;
                    for (int c = 0; c < ch; c++) buf[f * ch + c] = s;
                }
                Marshal.Copy(buf, 0, p, buf.Length);
            }

            render.ReleaseBuffer(avail, 0);
            written += (int)avail;
            iterations++;
            Thread.Sleep(2);
        }

        sb.AppendLine("wrote frames=" + written + " iterations=" + iterations);
        hr = client.Stop();
        sb.AppendLine("Stop hr=" + H(hr));
        return sb.ToString();
    }
}
}
'@

if (-not ('T2WasapiTone.Tone' -as [type])) {
    Add-Type -TypeDefinition $code
}

[T2WasapiTone.Tone]::Run($DeviceId, $Seconds, $Frequency, $Amplitude)
