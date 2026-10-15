# Related tools


## HLAE Camera IO

To be used with mirv_camio / mirv_streams record cam.  
These plugins and scripts import HLAE camera motion data into various 3D applications, or export it back to HLAE.

**After Effects** Import Script: [HLAE CamIO To AE](https://github.com/xNWP/HLAE-CamIO-To-AE/releases) by [xNWP](https://github.com/xNWP)

**Nuke** Import Script: [HLAE CamIO To Nuke](https://github.com/Devostated/HLAE-CamIO-To-Nuke/releases) by [Devostated](https://github.com/Devostated)

**Davinci Resolve** Import Script: [HLAE CamIO to Davinci](https://github.com/ChetdeJong/HLAE_CamIO_To_Davinci/releases) by [ChetdeJong](https://github.com/ChetdeJong)

**Blender** Plugins: [afx-blender-scripts](https://github.com/advancedfx/afx-blender-scripts)

**Cinema4D** Import Script: [HLAE CamIO To Cinema4D](https://github.com/xNWP/HLAE-CamIO-To-C4D/releases) by [xNWP](https://github.com/xNWP)  
**Cinema4D** Export Script: [Cinema4D Camera To HLAE CamIO](https://github.com/xNWP/Cinema4D-Cam-2-HLAE-CamIO/releases) by [xNWP](https://github.com/xNWP)


## HLAE Camera Motion Data (old)

These plugins and scripts import HLAE camera motion data into various 3D applications, or export it back to HLAE.

**3DsMax** Import Plugin [HLAE Cam Importer 1.0.4](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/hlaecamimporter104.rar)  
**3DsMax** Export Plugin [HLAE Cam Exporter 1.0.0](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/hlaecamexporter100.zip)  
Documentation: [HLAE Cam Importer Wiki](https://github.com/advancedfx/advancedfx/wiki/HLAE-Cam-Importer)  
System requirements: HLAE 2.2.5.14 or newer, 3dsMax 5.x(?7.x / 2008?) or newer.  
Author: msthavoc

**After Effects** Import Script: [hlaebvh2AECam 1.5](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/HLAE_BVH_2_AE_Cam_center15.zip)  
**After Effects** Export Script: [AECam2hlaebvh 1.0](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/AE_CAM_2_HLAE_BVH_10_msthavoc.zip)  
Documentation: Video Tutorials  
Author: msthavoc

**Blender** Plugins: [afx-blender-scripts](https://github.com/advancedfx/afx-blender-scripts)

**Cinema 4D** Import Plugin: [hlcam2c4d 1.4](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/hlcam2c4d_1_4_msthavoc.zip)  
**Cinema 4D** Export Plugin: [c4d2hlcam 1.0](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/c4d2hlcam_10_msthavoc.rar)  
Documentation: [HLAE Cam 2 Cinema 4D Wiki](https://github.com/advancedfx/advancedfx/wiki/HLAE-Cam-2-Cinema4D)
System requirements: HLAE 2.2.5.14 or newer, Cinema 4D R9.5 or newer  
Author: msthavoc

**Source FilmMaker** [Plugins: afx-sfm-scripts](https://github.com/advancedfx/afx-sfm-scripts/releases/latest)

Writing your own plugin:  
The HLAE .bvh format is close to the [BioVision (BVH) format](http://www.cs.wisc.edu/graphics/Courses/cs-838-1999/Jeff/BVH.html) but not compatible with it. The only difference is the rotation order: HLAE uses XYZ, while BVH requires ZXY.


## Other

### Blender

* [afx-blender-scripts](https://github.com/advancedfx/afx-blender-scripts)
* [HLAE-CoordIO](https://github.com/Devostated/HLAE-CoordIO) by [Devo](https://github.com/Devostated)
  Allows you to copy transformation data from Blender to After Effects.

### Cinema4D

* [AGR-Cleanup-Tool](https://github.com/xNWP/AGR-Cleanup-Tool) by [xNWP](https://github.com/xNWP)  
  Aids in cleaning up Advanced Effects Game Recording (AGR) Files.
* [Cinema-4D-Source-Tools](https://github.com/xNWP/Cinema-4D-Source-Tools) by [xNWP](https://github.com/xNWP)  
  Cinema 4D plugin for importing, exporting and editing files made by Valve Corporation's Source Engine.
* [Crafty Material Fix 0.2](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/crafty_mat_fix02.rar) (older versions: [0.1](https://github.com/advancedfx/advancedfx-prop/raw/tool-files/crafty_mat_fix01.rar))  
  About: Automates fixing material and texture links for maps exported by Crafty.  
  Documentation: Video Tutorial  
  Author: msthavoc  

* [HLAELiveLink](https://github.com/xNWP/HLAELiveLink) by [xNWP](https://github.com/xNWP)  
  Controls the camera in Source games through HLAE's mirv_pgl function, from Cinema 4D.

### Source FilmMaker

* [afx-sfm-scripts](https://github.com/advancedfx/afx-sfm-scripts/releases/latest)

### Other

* [HLAE-Server](https://github.com/xNWP/HLAE-Server) by [xNWP](https://github.com/xNWP)  
  Managed CLR library that controls Half-Life Advanced Effects through the mirv_pgl function.
* [SourceModelFixer](https://github.com/xNWP/SourceModelFixer) by [xNWP](https://github.com/xNWP)  
  Deletes corrupt .vtx files from Source games e.g. CS:GO.


