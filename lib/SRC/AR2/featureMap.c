/*
 *  AR2/featureMap.c
 *  ARToolKit5
 *
 *  This file is part of ARToolKit.
 *
 *  ARToolKit is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  ARToolKit is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with ARToolKit.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As a special exception, the copyright holders of this library give you
 *  permission to link this library with independent modules to produce an
 *  executable, regardless of the license terms of these independent modules, and to
 *  copy and distribute the resulting executable under terms of your choice,
 *  provided that you also meet, for each linked independent module, the terms and
 *  conditions of the license of that module. An independent module is a module
 *  which is neither derived from nor based on this library. If you modify this
 *  library, you may extend this exception to your version of the library, but you
 *  are not obligated to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 *  Copyright 2015 Daqri, LLC.
 *  Copyright 2006-2015 ARToolworks, Inc.
 *
 *  Author(s): Hirokazu Kato, Philip Lamb
 *
 */

#include <AR/ar.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <AR2/config.h>
#include <AR2/featureSet.h>
#include <sys/time.h>
#include <assert.h>
typedef struct timeval perf_t;

static int make_template( ARUint8 *imageBW, int xsize, int ysize,
                          int cx, int cy, int ts1, int ts2, float  sd_thresh,
                          float  *template, float  *vlen );

inline static int get_similarity( ARUint8 *imageBW, int xsize, int ysize,
                           float  *template, float  vlen, int ts1, int ts2,
                           int cx, int cy, float  *sim);

inline static int get_similarity_tile( ARUint8 *imageBW, int xsize, int ysize,
                                       float *template, float vlen, int ts1, int ts2,
                                       int cx, int cy, float  *sim,int size_sim);

int ar2FreeFeatureMap( AR2FeatureMapT *featureMap )
{
    free( featureMap->map );
    free( featureMap);

    return 0;
}

int ar2SaveFeatureMap( char *filename, char *ext, AR2FeatureMapT *featureMap )
{
    FILE   *fp;

    char buf[512];
    sprintf(buf, "%s.%s", filename, ext);
    if( (fp=fopen(buf, "wb")) == NULL ) return -1;

    if( fwrite(&(featureMap->xsize), sizeof(featureMap->xsize), 1, fp) != 1 ) goto bailBadWrite;
    if( fwrite(&(featureMap->ysize), sizeof(featureMap->ysize), 1, fp) != 1 ) goto bailBadWrite;
    if( fwrite(featureMap->map, sizeof(float), (featureMap->xsize)*(featureMap->ysize), fp) != (featureMap->xsize)*(featureMap->ysize) ) goto bailBadWrite;

    fclose(fp);
    return 0;
    
bailBadWrite:
    ARLOGe("Error saving feature map: error writing data.\n");
    fclose(fp);
    return (-1);
}

AR2FeatureMapT *ar2ReadFeatureMap( char *filename, char *ext )
{
    AR2FeatureMapT  *featureMap;
    FILE            *fp;

    char buf[512];
    sprintf(buf, "%s.%s", filename, ext);
    if( (fp=fopen(buf, "rb")) == NULL ) return NULL;

    arMalloc( featureMap, AR2FeatureMapT, 1 );

    if( fread(&(featureMap->xsize), sizeof(featureMap->xsize), 1, fp) != 1 ) {
        fclose(fp);
        free(featureMap);
        return NULL;
    }

    if( fread(&(featureMap->ysize), sizeof(featureMap->ysize), 1, fp) != 1 ) {
        fclose(fp);
        free(featureMap);
        return NULL;
    }

    arMalloc( featureMap->map, float, (featureMap->xsize)*(featureMap->ysize) );

    if( fread(featureMap->map, sizeof(float), (featureMap->xsize)*(featureMap->ysize), fp) != (featureMap->xsize)*(featureMap->ysize) ) {
        free(featureMap->map);
        free(featureMap);
        fclose(fp);
        return NULL;
    }

    fclose(fp);

    return featureMap;
}

static void perf(perf_t * p)
{
  gettimeofday(p, NULL);
}

static double perf_diff_ms(const perf_t * begin, perf_t * end)
{
  end->tv_sec = end->tv_sec - begin->tv_sec;
  end->tv_usec = end->tv_usec - begin->tv_usec;
  if (end->tv_usec < 0) {
    (end->tv_sec)--;
    end->tv_usec += 1000000;
  }
  return (end->tv_sec*1000000 +end->tv_usec)/1000;
}

AR2FeatureMapT *ar2GenFeatureMap( AR2ImageT *image,
                                  int ts1, int ts2,
                                  int search_size1, int search_size2,
                                  float  max_sim_thresh, float  sd_thresh )
{
    AR2FeatureMapT  *featureMap;
    float           *fimage, *fp;
    float           *fimage2, *fp2;
    float           *template;
    ARUint8         *p;
    float           dx, dy;
    int             xsize, ysize;
    int             hist[1000], sum;
    int             i, j, k;
    float           vlen;
    float           max, sim;
    int             ii, jj;
    perf_t start, stop, start_2, stop_2;
    int TILE = 512;
    float tile_storage[TILE];

    perf(&start);

    xsize = image->xsize;
    ysize = image->ysize;
    arMalloc(fimage,   float,  xsize*ysize);
    arMalloc(fimage2,  float,  xsize*ysize);
    arMalloc(template, float , (ts1+ts2+1)*(ts1+ts2+1));

    fp2 = fimage2;
#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
    p = image->imgBWBlur[1];
#else
    p = image->imgBW;
#endif
    for( i = 0; i < xsize; i++ ) {*(fp2++) = -1.0f; p++;}
    for( j = 1; j < ysize-1; j++ ) {
        *(fp2++) = -1.0f; p++;
        for( i = 1; i < xsize-1; i++ ) {
            dx = ((int)(*(p-xsize+1)) - (int)(*(p-xsize-1))
               +  (int)(*(p      +1)) - (int)(*(p      -1))
               +  (int)(*(p+xsize+1)) - (int)(*(p+xsize-1))) / (float )(3.0f*256);
            dy = ((int)(*(p+xsize+1)) - (int)(*(p-xsize+1))
               +  (int)(*(p+xsize  )) - (int)(*(p-xsize  ))
               +  (int)(*(p+xsize-1)) - (int)(*(p-xsize-1))) / (float )(3.0f*256);
            *(fp2++) = (float)sqrtf( (dx*dx+dy*dy) / (float )2.0f );
            p++;
        }
        *(fp2++) = -1.0f; p++;
    }
    for( i = 0; i < xsize; i++ ) {*(fp2++) = -1.0f; p++;}


    sum = 0;
    for( i = 0; i < 1000; i++ ) hist[i] = 0;
    fp2 = fimage2 + xsize + 1;
    for( j = 1; j < ysize-1; j++ ) {
        for( i = 1; i < xsize-1; i++ ) {
            if( *fp2 > *(fp2-1) && *fp2 > *(fp2+1) && *fp2 > *(fp2-xsize) && *fp2 > *(fp2+xsize) ) {
                k = (int)(*fp2 * 1000.0f);
                if( k > 999 ) k = 999;
                if( k < 0   ) k = 0;
                hist[k]++;
                sum++;
            }
            fp2++;
        }
        fp2 += 2;
    }
    j = 0;
    for( i = 999; i >= 0; i-- ) {
        j += hist[i];
        if( (float )j / (float )(xsize*ysize) >= 0.02f ) break;
        //if( (float )j / (float )(xsize*ysize) >= 0.2f ) break;
    }
    k = i;
    ARLOGi("         ImageSize = %7d[pixel]\n", xsize*ysize);
    ARLOGi("Extracted features = %7d[pixel]\n", sum);
    ARLOGi(" Filtered features = %7d[pixel]\n", j);


    fp = fimage;
    fp2 = fimage2;

    for( i = 0; i < xsize; i++ ) {
        fp[i] = 1.0f;
    }

    fp2 +=xsize;
    fp+=xsize;

    for( j = 1; j < ysize-1; j++ ) {
        ARLOGi("\r%4d/%4d.", j+1, ysize); fflush(stdout);
        *(fp++) = 1.0f;
        fp2++;
        for( i = 1; i < xsize-1; i++ ) {
            if( *fp2 <= *(fp2-1) || *fp2 <= *(fp2+1) || *fp2 <= *(fp2-xsize) || *fp2 <= *(fp2+xsize) ) {
                *(fp++) = 1.0f;
                fp2++;
                continue;
            }
            if( (int)(*fp2 * 1000) < k ) {
                *(fp++) = 1.0f;
                fp2++;
                continue;
            }
#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
            if( make_template(image->imgBWBlur[1], xsize, ysize, i, j, ts1, ts2, sd_thresh, template, &vlen) < 0 ) {
#else
            if( make_template(image->imgBW, xsize, ysize, i, j, ts1, ts2, sd_thresh, template, &vlen) < 0 ) {
#endif
                *(fp++) = 1.0f;
                fp2++;
                continue;
            }

            max = -1.0f;
            int ii_iter;
            int dst;
            for( jj = -search_size1; jj <= search_size1; jj++ ) {
              for (ii = -search_size1; ii <= search_size1 ; ii++){
                if( ii*ii + jj*jj <= search_size2*search_size2 )
                    ii =-ii + 1;

                /* Check possible contiguous values */
                ii_iter = ii; 
                for(; ii_iter <= search_size1 && ii_iter < TILE+ii; ++ii_iter)
                  {
                    if (ii_iter*ii_iter + jj*jj <= search_size2*search_size2)
                      break;
                    if (j+jj- ts1 < 0 || j+jj + ts2 >= ysize || i+ii_iter - ts1 < 0 || i+ii_iter + ts2 >= xsize )
                      break;
                  }


                /* ii not usable */
                if (ii_iter == ii) continue;

                dst = abs(ii - ii_iter);


#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
                if( !(get_similarity(image->imgBWBlur[1], xsize, ysize, template, vlen, ts1, ts2, i+ii, j+jj, &sim) < 0 )) 
#else
                  /* Calculate each stencil */
                  get_similarity_tile(image->imgBW, xsize, ysize, template, vlen, ts1, ts2, i+ii, j+jj, tile_storage, dst);

#endif
                /* Take first max value found (stencil is guud)*/
                for(int abc = ii; abc < ii_iter; ++abc)
                    if( tile_storage[abc-ii] > max ) {
                      max = tile_storage[abc-ii];
                        if( max > max_sim_thresh ) break;
                    }
                if (dst > 1)
                  ii += dst - 1;
                /* break dance to the top */
                if( max > max_sim_thresh ) break;

                }
                if( max > max_sim_thresh ) break;
            }
            
            *(fp++) = (float)max; fp2++;
        }
            *(fp++) = 1.0f; fp2++;
    }
        
#pragma omp simd
  	for (i = 0; i < xsize; i++) {
  		fp[i] = 1.0f;
  	}

    ARLOGi("\n");
    free(fimage2);
    free(template);

    arMalloc( featureMap, AR2FeatureMapT, 1 );
    featureMap->map = fimage;
    featureMap->xsize = xsize;
    featureMap->ysize = ysize;

    perf(&stop);
    double time = perf_diff_ms(&start,&stop);
    fprintf(stderr,"\nTime genmap : %.3lf  sec\n\n",time/1000);
    return featureMap;
}


AR2FeatureCoordT *ar2SelectFeature( AR2ImageT *image, AR2FeatureMapT *featureMap,
                                    int ts1, int ts2, int search_size2, int occ_size,
                                    float  max_sim_thresh, float  min_sim_thresh, float  sd_thresh, int *num )
{
    AR2FeatureCoordT   *coord;
    float              *template, vlen;
    float              *fimage2, *fp1, *fp2;
    float              min_sim;
    float              sim, min, max;
    float              dpi;
    int                xsize, ysize;
    int                max_feature_num;
    int                cx, cy;
    int                i, j;

    if( image->xsize != featureMap->xsize || image->ysize != featureMap->ysize ) return NULL;

    xsize = image->xsize;
    ysize = image->ysize;
    dpi = image->dpi;
    arMalloc(template, float , (ts1+ts2+1)*(ts1+ts2+1));
    arMalloc(fimage2, float, xsize*ysize);
    fp1 = featureMap->map;
    fp2 = fimage2;
    for( i = 0; i < xsize*ysize; i++ ) {
        *(fp2++) = *(fp1++);
    }

    max_feature_num = (xsize/occ_size)*(ysize/occ_size);
    if( max_feature_num < 10 ) max_feature_num = 10;
    ARLOGi("Max feature = %d\n", max_feature_num);
    arMalloc( coord, AR2FeatureCoordT, max_feature_num );
    *num = 0;

    while( *num < max_feature_num ) {

        min_sim = max_sim_thresh;
        fp2 = fimage2;
        cx = cy = -1;
        for( j = 0; j < ysize; j++ ) {
            for( i = 0; i < xsize; i++ ) {
                if( *fp2 < min_sim ) {
                    min_sim = *fp2;
                    cx = i;
                    cy = j;
                }
                fp2++;
            }
        }
        if( cx == -1 ) break;

#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
        if( make_template( image->imgBWBlur[1], xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#else
        if( make_template( image->imgBW, xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#endif
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }
        if( vlen/(ts1+ts2+1) < sd_thresh ) {
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }

        min = 1.0f;
        max = -1.0f;
        for( j = -search_size2; j <= search_size2; j++ ) {
            for( i = -search_size2; i <= search_size2; i++ ) {
                if( i*i + j*j > search_size2*search_size2 ) continue;
                if( i == 0 && j == 0 ) continue;

#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
                if( get_similarity(image->imgBWBlur[1], xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, &sim) < 0 ) continue;
#else
                if( get_similarity(image->imgBW, xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, &sim) < 0 ) continue;
#endif

                if( sim < min ) {
                    min = sim;
                    if( min < min_sim_thresh && min < min_sim ) break;
                }
                if( sim > max ) {
                    max = sim;
                    if( max > 0.99f ) break;
                }
            }
            if( (min < min_sim_thresh && min < min_sim) || max > 0.99f ) break;
        }

        if( (min < min_sim_thresh && min < min_sim) || max > 0.99f ) {
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }

        coord[*num].x = cx;
        coord[*num].y = cy;
        coord[*num].mx = (float) cx          / dpi * 25.4f; // millimetres.
        coord[*num].my = (float)(ysize - cy) / dpi * 25.4f; // millimetres.
        coord[*num].maxSim = (float)min_sim;
        (*num)++;

        ARLOGi("%3d: (%3d,%3d) : %f min=%f max=%f, sd=%f\n", *num, cx, cy, min_sim, min, max, vlen/(ts1+ts2+1));
        for( j = -occ_size; j <= occ_size; j++ ) {
            for( i = -occ_size; i <= occ_size; i++ ) {
                if( cy+j < 0 || cy+j >= ysize || cx+i < 0 || cx+i >= xsize ) continue;

                fimage2[(cy+j)*xsize+(cx+i)] = 1.0f;
            }
        }
    }

    free( template );
    free( fimage2 );

    return coord;
}


AR2FeatureCoordT *ar2SelectFeature2( AR2ImageT *image, AR2FeatureMapT *featureMap,
                                     int ts1, int ts2, int search_size2, int occ_size,
                                     float  max_sim_thresh, float  min_sim_thresh, float  sd_thresh, int *num )
{
    AR2FeatureCoordT   *coord;
    float              *template, vlen;
    float              min_sim;
    float              sim, min, max;
    float              *fimage2, *fp1, *fp2;
    float              dpi;
    int                xdiv, ydiv, div_size;
    int                xsize, ysize;
    int                max_feature_num;
    int                cx, cy;
    int                i, j;
    int                ii;
    perf_t start, stop;
    int TILE = 512;
    float tile_storage[TILE];
    perf(&start);
    
    if( image->xsize != featureMap->xsize || image->ysize != featureMap->ysize ) return NULL;

    occ_size *= 2;

    xsize = image->xsize;
    ysize = image->ysize;
    dpi = image->dpi;
    arMalloc(template, float , (ts1+ts2+1)*(ts1+ts2+1));
    arMalloc(fimage2, float, xsize*ysize);
    fp1 = featureMap->map;
    fp2 = fimage2;
    for( i = 0; i < xsize*ysize; i++ ) {
        *(fp2++) = *(fp1++);
    }

    div_size = (ts1+ts2+1)*3;
    xdiv = xsize/div_size;
    ydiv = ysize/div_size;

    max_feature_num = (xsize/occ_size)*(ysize/occ_size) + xdiv*ydiv;
    ARLOGi("Max feature = %d\n", max_feature_num);
    arMalloc( coord, AR2FeatureCoordT, max_feature_num );
    *num = 0;

    int nb_threads = 4;
    int *i_tab = calloc(nb_threads,sizeof(int));
    int *j_tab = calloc(nb_threads,sizeof(int));
    float *min_tab = calloc(nb_threads,sizeof(float));
    fp2 = fimage2;

    while( *num < max_feature_num ) {

        /* Each thread search for a local min */
#pragma omp parallel shared(min_tab,j_tab,i_tab,fp2) num_threads(nb_threads)
        {
          int thread_num = omp_get_thread_num();
          min_tab[thread_num] = max_sim_thresh;
          i_tab[thread_num] = j_tab[thread_num] = -1;
#pragma omp for
            for(int j_t = 0; j_t < ysize; j_t++ ) {
              for(int i_t = 0; i_t < xsize; i_t++ ) {
                if( fp2[i_t+j_t*xsize] < min_tab[thread_num] ) {
                  min_tab[thread_num] = fp2[i_t+j_t*xsize];
                  i_tab[thread_num] = i_t;
                  j_tab[thread_num] = j_t;
                }
              }
            }

        }
        /* Find which thread got the real min */
        int min_thread = 0;
        for(int nthread = 0; nthread < nb_threads ; ++nthread)
          if (min_tab[nthread] < min_tab[min_thread])
            min_thread = nthread;

        cx = i_tab[min_thread];
        cy = j_tab[min_thread];
        min_sim = min_tab[min_thread];
        /* useless       fp2 = &fimage2[cy*xsize+cx-1];*/

        if( cx == -1 ) break;

#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
        if( make_template( image->imgBWBlur[1], xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#else
        if( make_template( image->imgBW, xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#endif
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }
        if( vlen/(ts1+ts2+1) < sd_thresh ) {
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }

        min = 1.0f;
        max = -1.0f;
        int i_iter;
        int dst;

        for( j = -search_size2; j <= search_size2; j++ ) {
            for( i = -search_size2; i <= search_size2; i++ ) {

              i_iter = i;
              for(; i_iter <= search_size2 && i_iter < TILE+i; ++i_iter)
                {

                  if( i_iter*i_iter + j*j > search_size2*search_size2 ) break;
                  if( i_iter == 0 && j == 0 ) break;
                  if( cy+j - ts1 < 0 || cy+j + ts2 >= ysize || cx+i_iter - ts1 < 0 || cx+i_iter + ts2 >= xsize ) break;

                }
              /* i not usable */
              if (i_iter == i) continue;
              dst = abs(i - i_iter);
              

#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
                if( get_similarity(image->imgBWBlur[1], xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, &sim) < 0 ) continue;
#else
                get_similarity_tile(image->imgBW, xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, tile_storage, dst);

#endif

                /* Take first max value found (stencil is guud)*/
                for(int abc = i; abc < i_iter; ++abc)
                  {
                    if( tile_storage[abc-i] < min ) {
                      min = tile_storage[abc-i];
                      if( min < min_sim_thresh && min < min_sim ) break;
                    }
                    if( tile_storage[abc-i] > max ) {
                      max = tile_storage[abc-i];
                      if( max > 0.99f ) break;
                    }
                  }
                if (dst > 1)
                  i += dst - 1;
                if( (min < min_sim_thresh && min < min_sim) || max > 0.99f ) break;
            }
            if( (min < min_sim_thresh && min < min_sim) || max > 0.99f ) break;
        }

        if( (min < min_sim_thresh && min < min_sim) || max > 0.99f ) {
            fimage2[cy*xsize+cx] = 1.0f;
            continue;
        }

        coord[*num].x = cx;
        coord[*num].y = cy;
        coord[*num].mx = (float) cx          / dpi * 25.4f;
        coord[*num].my = (float)(ysize - cy) / dpi * 25.4f;
        coord[*num].maxSim = (float)min_sim;
        (*num)++;

        ARLOGi("%3d: (%3d,%3d) : %f min=%f max=%f, sd=%f\n", *num, cx, cy, min_sim, min, max, vlen/(ts1+ts2+1));
        for( j = -occ_size; j <= occ_size; j++ ) {
            for( i = -occ_size; i <= occ_size; i++ ) {
                if( cy+j < 0 || cy+j >= ysize || cx+i < 0 || cx+i >= xsize ) continue;

                fimage2[(cy+j)*xsize+(cx+i)] = 1.0f;
            }
        }
    }

 
    fp1 = featureMap->map;
    fp2 = fimage2;
    for( i = 0; i < xsize*ysize; i++ ) {
        *(fp2++) = *(fp1++);
    }
    for( ii = 0; ii < *num; ii++ ) {
        cx = coord[ii].x;
        cy = coord[ii].y;
        for( j = -occ_size; j <= occ_size; j++ ) {
            for( i = -occ_size; i <= occ_size; i++ ) {
                if( cy+j < 0 || cy+j >= ysize || cx+i < 0 || cx+i >= xsize ) continue;
                fimage2[(cy+j)*xsize+(cx+i)] = 1.0f;
            }
        }
    }

    ARLOGi("---------------------------------------------------------------\n");

    free( template );
    free( fimage2 );

    perf(&stop);
    double time = perf_diff_ms(&start,&stop);
    fprintf(stderr,"\nTime select feature : %.3lf  sec\n\n",time/1000);
    
    return coord;
}


int ar2PrintFeatureInfo( AR2ImageT *image, AR2FeatureMapT *featureMap, int ts1, int ts2, int search_size2, int cx, int cy )
{
    float       *template, vlen;
    float       max, min, sim;
    int         xsize, ysize;
    int         i, j;

    if( image->xsize != featureMap->xsize || image->ysize != featureMap->ysize ) return -1;

    arMalloc(template, float, (ts1+ts2+1)*(ts1+ts2+1));
    xsize = image->xsize;
    ysize = image->ysize;
    if( cx < 0 || cy < 0 || cx >= xsize || cy >= ysize ) {
        free(template);
        return -1;
    }

    if( featureMap->map[cy*xsize+cx] == 1.0 ) {
        ARLOG("%3d, %3d: max_sim = %f\n", cx, cy, featureMap->map[cy*xsize+cx]);
        free( template );
        return 0;
    }

#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
    if( make_template( image->imgBWBlur[1], xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#else
    if( make_template( image->imgBW, xsize, ysize, cx, cy, ts1, ts2, 0.0, template, &vlen ) < 0 ) {
#endif
        free( template );
        return -1;
    }

    min = 1.0f;
    max = -1.0f;
ARLOG("\n");
    for( j = -search_size2; j <= search_size2; j++ ) {
        for( i = -search_size2; i <= search_size2; i++ ) {
#if AR2_CAPABLE_ADAPTIVE_TEMPLATE
            if( get_similarity(image->imgBWBlur[1], xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, &sim) < 0 ) continue;
#else
            if( get_similarity(image->imgBW, xsize, ysize, template, vlen, ts1, ts2, cx+i, cy+j, &sim) < 0 ) continue;
#endif

            if( (i*i + j*j <= search_size2*search_size2) 
             && (i != 0 || j != 0) ) {
                if( sim < min ) min = sim;
                if( sim > max ) max = sim;
            }
ARLOG("%7.4f ", sim);
        }
ARLOG("\n");
    }
ARLOG("\n");

    ARLOG("%3d, %3d: max_sim = %f, (max,min) = %f, %f, sd = %f\n", cx, cy, featureMap->map[cy*xsize+cx], max, min, vlen/(ts1+ts2+1));
    free( template );
    return 0;
}


static int make_template( ARUint8 *imageBW, int xsize, int ysize,
                          int cx, int cy, int ts1, int ts2, float  sd_thresh,
                          float *template, float *vlen )
{
    ARUint8  *ip;
    float    *tp;
    float     vlen1, ave;
    int       i, j;

    if( cy - ts1 < 0 || cy + ts2 >= ysize || cx - ts1 < 0 || cx + ts2 >= xsize ) return -1;

    ave = 0.0f;


    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        #pragma omp simd
        for( i = 0; i <= ts1+ts2-4 ; i+=4 )
          {
            ave += ip[i];
            ave += ip[i+1];
            ave += ip[i+2];
            ave += ip[i+3];

          }
        for (; i <= ts1+ts2; ++i)
          {
            ave += ip[i];
          }
    }
    ave /= (ts1+ts2+1)*(ts1+ts2+1);

    tp = template;
    vlen1 = 0.0f;
    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        #pragma omp simd
        for( i = 0; i <= ts1+ts2 ; i++ ) {
          tp[i+(j+ts1)*(ts1+ts2+1)] = (float )ip[i] - ave;
          vlen1 += tp[i+(j+ts1)*(ts1+ts2+1)] * tp[i+(j+ts1)*(ts1+ts2+1)];
        }
    }
    
    if( vlen1 == 0.0f ) return -1;
    if( vlen1/((ts1+ts2+1)*(ts1+ts2+1)) < sd_thresh*sd_thresh ) return -1;
    
    *vlen = sqrtf(vlen1);

    return 0;
}

inline static int get_similarity( ARUint8 *imageBW, int xsize, int ysize,
                           float *template, float vlen, int ts1, int ts2,
                                  int cx, int cy, float  *sim)
{
#if 0
    ARUint8   *ip;
    float     *tp;
    float     ave2, w1, w2, vlen2;
    int       i, j;
    

    if( cy - ts1 < 0 || cy + ts2 >= ysize || cx - ts1 < 0 || cx + ts2 >= xsize ) return -1;

    
    ave2 = 0.0f;
    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        for( i = -ts1; i <= ts2 ; i++ ) ave2 += *(ip++);
    }
    ave2 /= (ts1+ts2+1)*(ts1+ts2+1);

    tp = template;
    w1 = 0.0f;
    vlen2 = 0.0f;
    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        for( i = -ts1; i <= ts2 ; i++ ) {
            w2 = (float )(*(ip++)) - ave2;
            vlen2 += w2 * w2;
            w1 += *(tp++) * w2;
        }
    }
    if( vlen2 == 0.0f ) return -1;

    vlen2 = sqrtf(vlen2);
    *sim = w1 / (vlen * vlen2);
#else
    ARUint8   *ip;
    float     *tp;
    float     sx, sxx, sxy;
    float     vlen2;
    int       i, j;

    if( cy - ts1 < 0 || cy + ts2 >= ysize || cx - ts1 < 0 || cx + ts2 >= xsize ) {*sim = -1;return -1;}

    tp = template;
    sx = sxx = sxy = 0.0f;
    //#pragma omp parallel for private(i) firstprivate(tp,ip) reduction(+:sx) reduction(+:sxx) reduction(+:sxy)
    for(int j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        #pragma omp simd
        for(i = 0; i <= ts1+ts2-4 ; i+=4 ) {
          sx += ip[i]+ip[i+1]+ip[i+2]+ip[i+3];
          sxx += (ip[i] * ip[i]) + (ip[i+1] * ip[i+1]) + (ip[i+2] * ip[i+2]) + (ip[i+3] * ip[i+3]);
          sxy += ip[i] * tp[i+(j+ts1)*(ts1+ts2+1)] + ip[i+1] * tp[i+1+(j+ts1)*(ts1+ts2+1)] +
            ip[i+2] * tp[i+2+(j+ts1)*(ts1+ts2+1)] + ip[i+3] * tp[i+3+(j+ts1)*(ts1+ts2+1)];
        }
        for(; i <= ts1+ts2 ; ++i ) {
          sx += ip[i];
          sxx += ip[i] * ip[i];
          sxy += ip[i] * tp[i+(j+ts1)*(ts1+ts2+1)];
        }

    }

    vlen2 = sxx - sx*sx/((ts1+ts2+1)*(ts1+ts2+1));
    if( vlen2 == 0.0f )
      {
        *sim = -1;
      return -1;
      }
    vlen2 = sqrtf(vlen2);

    *sim = sxy / (vlen * vlen2);
#endif
    
    return 0;
}


inline static int get_similarity_tile( ARUint8 *imageBW, int xsize, int ysize,
                           float *template, float vlen, int ts1, int ts2,
                                  int cx, int cy, float  *sim,int size_sim)
{
#if 0
    ARUint8   *ip;
    float     *tp;
    float     ave2, w1, w2, vlen2;
    int       i, j;
    

    /*XXX: You have to check manually you are inside with each element before */
    //if( cy - ts1 < 0 || cy + ts2 >= ysize || cx - ts1 < 0 || cx + ts2 >= xsize ) return -1;

    
    ave2 = 0.0f;
    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        for( i = -ts1; i <= ts2 ; i++ ) ave2 += *(ip++);
    }
    ave2 /= (ts1+ts2+1)*(ts1+ts2+1);

    tp = template;
    w1 = 0.0f;
    vlen2 = 0.0f;
    for( j = -ts1; j <= ts2; j++ ) {
        ip = &imageBW[(cy+j)*xsize+(cx-ts1)];
        for( i = -ts1; i <= ts2 ; i++ ) {
            w2 = (float )(*(ip++)) - ave2;
            vlen2 += w2 * w2;
            w1 += *(tp++) * w2;
        }
    }
    if( vlen2 == 0.0f ) return -1;

    vlen2 = sqrtf(vlen2);
    *sim = w1 / (vlen * vlen2);
#else
    ARUint8   *ip;
    float     *tp;
    float     sx, sxx, sxy;
    float     vlen2;
    int       i, j;
    int nbthread = 4;
    tp = template;

    if(size_sim < 4)
      nbthread = 1;

#pragma omp parallel for firstprivate(sx,sxx,sxy,ip,vlen2,vlen,cx) private(i) num_threads(nbthread) 
    for(int tile = 0; tile < size_sim; ++tile)
      {
        int ccx = cx + tile;
        sx = sxx = sxy = 0.0f;
        for(int j = 0; j <= ts1+ts2; j++ ) {
          ip = &imageBW[(cy+j-ts1)*xsize+(ccx-ts1)];

          for(i = 0; i <= ts1+ts2-4 ; i+=4 ) {
            sx += ip[i]+ip[i+1]+ip[i+2]+ip[i+3];
            sxx += (ip[i] * ip[i]) + (ip[i+1] * ip[i+1]) + (ip[i+2] * ip[i+2]) + (ip[i+3] * ip[i+3]);
            sxy += ip[i] * tp[i+(j)*(ts1+ts2+1)] + ip[i+1] * tp[i+1+(j)*(ts1+ts2+1)] +
              ip[i+2] * tp[i+2+(j)*(ts1+ts2+1)] + ip[i+3] * tp[i+3+(j)*(ts1+ts2+1)];
          }
          for(; i <= ts1+ts2 ; ++i ) {
            sx += ip[i];
            sxx += ip[i] * ip[i];
            sxy += ip[i] * tp[i+(j)*(ts1+ts2+1)];
          }

        }

        vlen2 = sxx - sx*sx/((ts1+ts2+1)*(ts1+ts2+1));
        if( vlen2 == 0.0f )
          {
            sim[tile] = -1;
            continue;
          }
        vlen2 = sqrtf(vlen2);

        sim[tile] = sxy / (vlen * vlen2);
#endif
      }
    return 0;
}
