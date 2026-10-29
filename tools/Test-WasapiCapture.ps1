# T2AudioPort - WASAPI capture probe for Microphone Array testing.
param(
    [int]$DurationSeconds = 5
)

$ErrorActionPreference = 'Stop'

$code = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace T2Capture {
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
    [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out IMMDeviceCollection devices);
    [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice endpoint);
    [PreserveSig] int GetDevice(string pwstrId, out IMMDevice device);
}

[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceCollection {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int Item(uint index, out IMMDevice device);
}

[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid id, CLSCTX clsCtx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    [PreserveSig] int OpenPropertyStore(uint stgmAccess, out IntPtr properties);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string strId);
    [PreserveSig] int GetState(out uint state);
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

public class Runner {
    static string H(int hr) { return "0x" + hr.ToString("X8"); }

    public static string Run(int seconds) {
        var sb = new StringBuilder();
        var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();
        IMMDeviceCollection col;
        int hr = enumerator.EnumAudioEndpoints(EDataFlow.eCapture, 1, out col);
        if (hr != 0 || col == null) return "EnumAudioEndpoints hr=" + H(hr);

        uint count;
        col.GetCount(out count);
        sb.AppendLine("Capture devices found: " + count);

        IMMDevice t2Mic = null;
        for (uint i = 0; i < count; i++) {
            IMMDevice dev;
            col.Item(i, out dev);
            if (dev == null) continue;
            string id;
            dev.GetId(out id);
            sb.AppendLine("  [" + i + "] " + id);
            if (id.Contains("7F162DDA") || id.Contains("Apple") || t2Mic == null) {
                t2Mic = dev;
            }
        }

        if (t2Mic == null) return sb.ToString() + "\nNo capture device found.";

        Guid iid = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        object o;
        hr = t2Mic.Activate(ref iid, CLSCTX.ALL, IntPtr.Zero, out o);
        sb.AppendLine("Activate(IAudioClient) hr=" + H(hr));
        if (hr != 0 || o == null) return sb.ToString();

        var client = (IAudioClient)o;
        IntPtr pfmt;
        hr = client.GetMixFormat(out pfmt);
        sb.AppendLine("GetMixFormat hr=" + H(hr));
        if (hr == 0 && pfmt != IntPtr.Zero) {
            var fmt = (WAVEFORMATEXTENSIBLE)Marshal.PtrToStructure(pfmt, typeof(WAVEFORMATEXTENSIBLE));
            sb.AppendLine(string.Format("MixFormat: ch={0} rate={1} bits={2} mask=0x{3:X8}",
                fmt.Format.nChannels, fmt.Format.nSamplesPerSec, fmt.Format.wBitsPerSample, fmt.dwChannelMask));
        }

        hr = client.Initialize(0, 0, 10000000, 0, pfmt, IntPtr.Zero);
        sb.AppendLine("Initialize(shared) hr=" + H(hr));
        if (hr != 0) return sb.ToString();

        uint frames;
        hr = client.GetBufferSize(out frames);
        sb.AppendLine("GetBufferSize hr=" + H(hr) + " frames=" + frames);

        sb.AppendLine("Starting capture for " + seconds + " seconds (MicStartIo=0 expected)...");
        hr = client.Start();
        sb.AppendLine("Start hr=" + H(hr));

        System.Threading.Thread.Sleep(seconds * 1000);

        hr = client.Stop();
        sb.AppendLine("Stop hr=" + H(hr));
        sb.AppendLine("Capture test completed safely without crash.");
        return sb.ToString();
    }
}
}
'@

if (-not ('T2Capture.Runner' -as [type])) {
    Add-Type -TypeDefinition $code
}

[T2Capture.Runner]::Run($DurationSeconds)
