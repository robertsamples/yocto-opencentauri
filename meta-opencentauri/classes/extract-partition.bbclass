SOURCE_IMAGE_FILE ?= "${DEPLOY_DIR_IMAGE}/${IMAGE_LINK_NAME}.wic"
EXTRACT_PARTITION_LABELS ?= "boot"

do_extract_partition[nostamp] = "1"

python do_extract_partition() {
    import subprocess, os

    source = d.getVar('SOURCE_IMAGE_FILE')
    deploy_dir = d.getVar('DEPLOY_DIR_IMAGE')
    labels = d.getVar('EXTRACT_PARTITION_LABELS').split()

    if os.path.islink(source):
        source = os.path.join(os.path.dirname(source), os.readlink(source))

    result = subprocess.run(
        ['partx', '--output', 'NR,START,END', '--pairs', source],
        text=True, capture_output=True
    )
    if result.returncode != 0:
        bb.fatal("partx failed: %s" % result.stderr)

    for line in result.stdout.splitlines():
        pairs = {k: v.strip('"') for k, v in (kv.split('=') for kv in line.split())}
        start = int(pairs['START'])
        end = int(pairs['END'])
        offset = start * 512

        blkid = subprocess.run(
            ['blkid', '-p', '-O', str(offset), '-o', 'export', source],
            text=True, capture_output=True
        )

        blkid_vars = dict(line.split('=', 1) for line in blkid.stdout.splitlines() if '=' in line)
        label = blkid_vars.get('LABEL')
        if label in labels:
            labels.remove(label)
            count = end - start + 1
            out = os.path.join(deploy_dir, label + '.img')
            subprocess.check_call([
                'dd', 'if=' + source, 'of=' + out,
                'bs=512', 'skip=' + str(start), 'count=' + str(count)
            ])
            bb.note("Extracted partition '%s' to %s" % (label, out))

    if labels:
        bb.fatal("Could not find partition with label '%s'" % label)
}

addtask do_extract_partition after do_image_complete before do_build
