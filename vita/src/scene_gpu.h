/* scene_gpu.h -- per-GXM-scene GPU time (scene_gpu.c). Off unless ux0:data/stairfax/scene_gpu.txt asks. */
#ifndef SCENE_GPU_H
#define SCENE_GPU_H

void scene_gpu_init(void);            /* once, at boot: reads the markers, starts the poll thread if asked */
void scene_gpu_present(int present);  /* after every present: closes the frame, aggregates the finished ones */
void scene_gpu_report(int present);   /* the [scene] block for the window since the last call */
void scene_gpu_shutdown(void);        /* asks the poll thread to stop, waits <= 1.5 s, never kills it */

#endif
