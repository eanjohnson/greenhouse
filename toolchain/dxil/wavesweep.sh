#!/bin/bash
# Finish the #175 intrinsic table. Ordered by how many corpus blobs each cell
# unblocks (from _wavecensus.json), so a short run still buys the most.
#   quad 27 | ballot 6 | quadat 5 | countbits 5 | prefixbits 4 | readlaneat 2
#   activebit / match / alltrue / allequal: 0-1 blobs, they only close the table.
# DO NOT RUN while the console hold is in force.
cd $HOME/.claude/jobs/49fb5998/tmp
export PYTHONPATH=$PWD LC_ALL=C
for n in quad ballot quadat countbits prefixbits readlaneat activebit match alltrue allequal; do
  [ -f _wavectl/${n}_s.bin ] || { echo "$n: no control blob" >> _wavesweep2.txt; continue; }
  GH_TAG=ctl_$n GH_DXIL_CTL_BIN=_wavectl/${n}_s.bin GH_WINDOW=70 GH_PICK_BY=999 GH_POLL_TIMEOUT=45 \
    python3 -P run286.py $(cat _runargs50.txt) > _gt_ctl2_$n.out 2>&1
  cp _d3.txt _d_ctl2_$n.txt
  printf '%-12s ' $n >> _wavesweep2.txt
  /usr/bin/grep -a 'DXILCTL CreateComputePipelineState' _d_ctl2_$n.txt | tail -1 >> _wavesweep2.txt
done
echo SWEEPDONE >> _wavesweep2.txt
