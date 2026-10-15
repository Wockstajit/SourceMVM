# Record the CS2 client surface with the same PrintWindow path as capture-game-window.ps1.
# GDI title capture returns black for this DirectX window. No desktop pixels are captured.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [double]$Seconds=8,
    [int]$Fps=30
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
public static class MotionCapture {
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int L,T,R,B; }
    [StructLayout(LayoutKind.Sequential)] struct Point { public int X,Y; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr h,ref Point p);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    public static void Record(IntPtr h,string ffmpeg,string path,double seconds,int fps) {
        SetThreadDpiAwarenessContext(new IntPtr(-4));
        Rect w,c; Point origin=new Point();
        GetWindowRect(h,out w); GetClientRect(h,out c); ClientToScreen(h,ref origin);
        int width=c.R-c.L,height=c.B-c.T;
        if(width<=0 || height<=0 || fps<1 || fps>60 || seconds<=0 || seconds>60) throw new Exception("Invalid capture bounds");
        string args="-y -hide_banner -loglevel warning -f rawvideo -pixel_format bgra -video_size "+width+"x"+height+" -framerate "+fps+" -i pipe:0 -an -vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p \""+path+"\"";
        var start=new ProcessStartInfo(ffmpeg,args) { UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true,RedirectStandardError=true };
        using(var encoder=Process.Start(start)) {
            var errors=encoder.StandardError.ReadToEndAsync();
            using(var full=new Bitmap(w.R-w.L,w.B-w.T,PixelFormat.Format32bppArgb))
            using(var graphics=Graphics.FromImage(full))
            using(var csv=new StreamWriter(path+".frames.csv")) {
                csv.WriteLine("frame,wall_seconds");
                var clock=Stopwatch.StartNew();
                var region=new Rectangle(origin.X-w.L,origin.Y-w.T,width,height);
                byte[] bytes=new byte[width*height*4];
                int count=(int)Math.Ceiling(seconds*fps);
                for(int frame=0;frame<count;frame++) {
                    double due=(double)frame/fps;
                    int wait=(int)((due-clock.Elapsed.TotalSeconds)*1000);
                    if(wait>0) Thread.Sleep(wait);
                    IntPtr dc=graphics.GetHdc();
                    bool ok;
                    try { ok=PrintWindow(h,dc,2); } finally { graphics.ReleaseHdc(dc); }
                    if(!ok) throw new Exception("PrintWindow failed");
                    using(var client=full.Clone(region,PixelFormat.Format32bppArgb)) {
                        var data=client.LockBits(new Rectangle(0,0,width,height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
                        try { Marshal.Copy(data.Scan0,bytes,0,bytes.Length); } finally { client.UnlockBits(data); }
                    }
                    csv.WriteLine(frame+","+clock.Elapsed.TotalSeconds.ToString("F6",System.Globalization.CultureInfo.InvariantCulture));
                    encoder.StandardInput.BaseStream.Write(bytes,0,bytes.Length);
                }
            }
            encoder.StandardInput.Close();
            if(!encoder.WaitForExit(15000)) throw new Exception("Encoder did not finish");
            File.WriteAllText(path+".log",errors.Result);
            if(encoder.ExitCode!=0) throw new Exception("Encoder failed: "+errors.Result);
        }
    }
}
'@
$game=Get-Process cs2 | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if(!$game) { throw 'No visible CS2 window.' }
$path=[IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
[MotionCapture]::Record($game.MainWindowHandle,(Get-Command ffmpeg -ErrorAction Stop).Source,$path,$Seconds,$Fps)
Write-Output "Recorded CS2 client surface: $path"
