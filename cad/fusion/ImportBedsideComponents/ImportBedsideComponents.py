"""Fusion script: bring the bedside's component models into Fusion.

1. Uploads every .f3d in cad/components/ to a "Bedside components" folder in
   the active Fusion project (skipping ones already there).
2. Inserts each one into the open design, laid out in a row along X with a
   20 mm gap, so you can drag them into place.

Run it from Utilities > ADD-INS > Scripts and Add-Ins (link this folder with
the + button first). Open the design you want them in before running.
"""

import os
import traceback

import adsk.core
import adsk.fusion

HERE = os.path.dirname(os.path.realpath(__file__))
COMPONENTS = os.path.normpath(os.path.join(HERE, '..', '..', 'components'))
PROJECT_FOLDER = 'Bedside components'
GAP_CM = 2.0  # Fusion's API works in cm


def upload_to_project(app, files):
    project = app.data.activeProject
    if project is None:
        return 'No active project, skipped the upload.'
    root = project.rootFolder
    folder = root.dataFolders.itemByName(PROJECT_FOLDER) or root.dataFolders.add(PROJECT_FOLDER)
    existing = {folder.dataFiles.item(i).name for i in range(folder.dataFiles.count)}
    sent = []
    for path in files:
        name = os.path.splitext(os.path.basename(path))[0]
        if name not in existing:
            folder.uploadFile(path)
            sent.append(name)
    return 'Uploading {} new file(s) to {} / {} (runs in the background).'.format(
        len(sent), project.name, PROJECT_FOLDER)


def insert_into_design(app, design, files):
    root = design.rootComponent
    im = app.importManager
    x = 0.0
    for path in files:
        im.importToTarget(im.createFusionArchiveImportOptions(path), root)
        occ = root.occurrences.item(root.occurrences.count - 1)
        bb = occ.boundingBox
        m = adsk.core.Matrix3D.create()
        m.translation = adsk.core.Vector3D.create(x - bb.minPoint.x, -bb.minPoint.y, -bb.minPoint.z)
        occ.transform2 = m
        x += (bb.maxPoint.x - bb.minPoint.x) + GAP_CM
    if design.designType == adsk.fusion.DesignTypes.ParametricDesignType:
        try:
            design.snapshots.add()  # keep the positions in the timeline
        except Exception:
            pass
    return 'Inserted {} component(s) into the open design.'.format(len(files))


def run(context):
    ui = None
    try:
        app = adsk.core.Application.get()
        ui = app.userInterface
        files = sorted(os.path.join(COMPONENTS, f) for f in os.listdir(COMPONENTS)
                       if f.lower().endswith('.f3d'))
        if not files:
            ui.messageBox('No .f3d files in ' + COMPONENTS)
            return
        lines = [upload_to_project(app, files)]
        design = adsk.fusion.Design.cast(app.activeProduct)
        if design:
            lines.append(insert_into_design(app, design, files))
        else:
            lines.append('No design open, so nothing was inserted.')
        ui.messageBox('\n'.join(lines))
    except Exception:
        if ui:
            ui.messageBox('Failed:\n' + traceback.format_exc())
