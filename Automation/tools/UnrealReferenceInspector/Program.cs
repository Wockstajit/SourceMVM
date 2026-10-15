using UAssetAPI;
using UAssetAPI.ExportTypes;
using UAssetAPI.UnrealTypes;
using System.Text.Json;
using UAssetAPI.PropertyTypes.Objects;
using UAssetAPI.PropertyTypes.Structs;
using System.Security.Cryptography;
if(args.Length!=2) throw new Exception("usage: ue-decode content-directory output-directory");
Directory.CreateDirectory(args[1]);
var report = new List<object>();
foreach(var file in Directory.EnumerateFiles(args[0],"*.uasset",SearchOption.AllDirectories)) {
    var relative=Path.GetRelativePath(args[0],file);
    if(!relative.StartsWith("MasterMaterial") && !relative.StartsWith("Widgets")) continue;
    if(relative.Contains("Textures") || relative.Contains("Icons")) continue;
    try {
        // Let the serialized package versions select the parser; never force a
        // newer package into an older engine version to make parsing succeed.
        var asset=new UAsset(file);
        if(asset.RecordedEngineVersion.Major==5 && asset.RecordedEngineVersion.Minor==8) asset=new UAsset(file, EngineVersion.VER_UE5_8);
        var version=$"{asset.RecordedEngineVersion.Major}.{asset.RecordedEngineVersion.Minor}.{asset.RecordedEngineVersion.Patch}";
        var raw=asset.Exports.Count(x=>x is RawExport);
        var output=Path.Combine(args[1],relative+".json");
        Directory.CreateDirectory(Path.GetDirectoryName(output)!);
        File.WriteAllText(output,asset.SerializeJson(true));
        var connections=new List<object>();
        string Reference(int index)=>index>0 && index<=asset.Exports.Count?asset.Exports[index-1].ObjectName.ToString():index<0 && -index<=asset.Imports.Count?asset.Imports[-index-1].ObjectName.ToString():"unresolved";
        void Walk(PropertyData property,int owner,string path){
            var reference=property.GetType().GetField("Expression")?.GetValue(property) as FPackageIndex;
            if(reference!=null){
                object? Field(string key)=>property.GetType().GetField(key)?.GetValue(property);
                connections.Add(new {owner,ownerName=Reference(owner),path,expression=reference.Index,expressionName=Reference(reference.Index),outputIndex=Field("OutputIndex"),mask=Field("Mask"),maskR=Field("MaskR"),maskG=Field("MaskG"),maskB=Field("MaskB"),maskA=Field("MaskA")});
            }
            if(property is StructPropertyData structure)foreach(var child in structure.Value)Walk(child,owner,path+"/"+child.Name);
            if(property is ArrayPropertyData array)foreach(var child in array.Value)Walk(child,owner,path+"/"+child.Name);
        }
        for(int i=0;i<asset.Exports.Count;i++)if(asset.Exports[i] is NormalExport normal)foreach(var property in normal.Data)Walk(property,i+1,property.Name.ToString());
        File.WriteAllText(output+".connections.json",JsonSerializer.Serialize(connections,new JsonSerializerOptions{WriteIndented=true}));
        report.Add(new {file=relative,version,engine=asset.GetEngineVersion().ToString(),exports=asset.Exports.Count,rawExports=raw,sha256=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))),materialConnections=connections.Count,complete=raw==0 && asset.RecordedEngineVersion.Minor<=7});
    } catch(Exception e) { report.Add(new {file=relative,error=e.Message}); }
}
File.WriteAllText(Path.Combine(args[1],"index.json"),JsonSerializer.Serialize(report,new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));
Console.WriteLine($"Inspected {report.Count} assets");
