#include "mission_state.h"
#include "incident.h"
#include "environment.h"
#include <stdlib.h>

void initialize_grid(void) //grid
{
    for(int x=0;x<GRID_WIDTH;x++)
    {
        for(int y=0;y<GRID_HEIGHT;y++)
        {
            g_grid[x][y].safety_level=environment_cell_base_safety(x, y);
        }
    }
}


static void degrade_grid_area(int centre_x,int centre_y,int radius,int base_degradation) //grid
{
    for(int x=centre_x-radius;x<=centre_x+radius;x++)
    {
        for(int y=centre_y-radius;y<=centre_y+radius;y++)
        {
            if(x<0 || x>=GRID_WIDTH || y<0 || y>=GRID_HEIGHT)
            {
                continue;
            }
            int distance=abs(x-centre_x)+abs(y-centre_y);
            if(distance>radius) continue;
            int degradation=base_degradation-base_degradation*distance/(radius+1);
            if(degradation<0) continue;
            int safety=g_grid[x][y].safety_level-=degradation;
            if(safety<0) g_grid[x][y].safety_level=0;
            else
            g_grid[x][y].safety_level=safety;
        }
    }
}

void apply_incident_grid_degradation(const IncidentState_t *incident) //grid
{
    int radius=incident_degradation_radius(incident->incident_type);
    int degradation=incident_base_degradation(incident->incident_type);
    if(radius==0 || degradation==0) return;
    degrade_grid_area(incident->loc_x,incident->loc_y,radius,degradation);

}


