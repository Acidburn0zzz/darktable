/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "gui/draw.h"

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING 2

GStaticMutex _lib_backgroundjobs_mutex = G_STATIC_MUTEX_INIT;

typedef struct dt_bgjob_t {
  uint32_t type;
  GtkWidget *widget,*progressbar,*label;
} dt_bgjob_t;

typedef struct dt_lib_backgroundjobs_t
{
  GtkWidget *jobbox;
  GHashTable *jobs;
}
dt_lib_backgroundjobs_t;

/* proxy function for creating a ui bgjob plate */
static guint _lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message);
/* proxy function for destroying a ui bgjob plate */
static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, guint key);
/* proxy function for assigning and set cancel job for a ui bgjob plate*/
static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, guint key, struct dt_job_t *job);
/* proxy function for setting the progress of a ui bgjob plate */
static void _lib_backgroundjobs_progress(dt_lib_module_t *self, guint key, double progress);
/* callback when cancel job button is pushed  */
static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data);

const char* name()
{
  return _("background jobs");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_DARKROOM | DT_VIEW_PANEL_LEFT | DT_VIEW_PANEL_BOTTOM;
}

int position()
{
  return 1;
}

int expandable()
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)g_malloc(sizeof(dt_lib_backgroundjobs_t));
  memset(d,0,sizeof(dt_lib_backgroundjobs_t));
  self->data = (void *)d;

  d->jobs = g_hash_table_new(NULL,NULL);

  /* initialize base */
  self->widget = d->jobbox = gtk_vbox_new(FALSE, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(self->widget), 5);

  /* setup proxy */
  darktable.control->proxy.backgroundjobs.module = self;
  darktable.control->proxy.backgroundjobs.create = _lib_backgroundjobs_create;
  darktable.control->proxy.backgroundjobs.destroy = _lib_backgroundjobs_destroy;
  darktable.control->proxy.backgroundjobs.progress = _lib_backgroundjobs_progress;
  darktable.control->proxy.backgroundjobs.set_cancellable = _lib_backgroundjobs_set_cancellable;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  darktable.control->proxy.backgroundjobs.module = NULL;

  g_free(self->data);
  self->data = NULL;
}

static guint _lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message)
{
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)self->data;

  /* lets make this threadsafe */
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();

  /* initialize a new job */
  dt_bgjob_t *j=(dt_bgjob_t*)g_malloc(sizeof(dt_bgjob_t));
  j->type = type;
  j->widget = gtk_event_box_new();

  guint key = g_direct_hash(j);
  g_hash_table_insert(d->jobs, (gpointer)key, j);

  /* intialize the ui elements for job */
  gtk_widget_set_name (GTK_WIDGET (j->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX (gtk_vbox_new (FALSE,0));
  GtkBox *hbox = GTK_BOX (gtk_hbox_new (FALSE,0));
  gtk_container_set_border_width (GTK_CONTAINER(vbox),2);
  gtk_container_add (GTK_CONTAINER(j->widget), GTK_WIDGET(vbox));

  /* add job label */
  j->label = gtk_label_new(message);
  gtk_misc_set_alignment(GTK_MISC(j->label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( hbox ), GTK_WIDGET(j->label), TRUE, TRUE, 0);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if (type == 0)
  {
    j->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start( GTK_BOX( vbox ), j->progressbar, TRUE, FALSE, 2);
  }

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(d->jobbox), j->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child(GTK_BOX(d->jobbox), j->widget, 1);
  gtk_widget_show_all(j->widget);
  gtk_widget_show(d->jobbox);
  
  if(needlock) gdk_threads_leave();
  return key;
}

static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, guint key)
{
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, (gpointer)key);
  if(j) 
  {
    g_hash_table_remove(d->jobs, (gpointer)key);
    
    /* remove job widget from jobbox */
    if(GTK_IS_WIDGET(j->widget))
      gtk_container_remove(GTK_CONTAINER(d->jobbox),j->widget);

    /* if jobbox is empty lets hide */
    if(g_list_length(gtk_container_get_children(GTK_CONTAINER(d->jobbox)))==0)
      gtk_widget_hide(d->jobbox);
    
    /* free allocted mem */
    g_free(j);
  }
  if(needlock) gdk_threads_leave();
}

static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data)
{
  dt_job_t *job=(dt_job_t *)user_data;
  dt_control_job_cancel(job);
}

static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, guint key, struct dt_job_t *job)
{
  if(!darktable.control->running) return;
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, (gpointer)key);
  if (j)
  {
    GtkWidget *w=j->widget;
    GtkBox *hbox = GTK_BOX (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER ( gtk_bin_get_child (GTK_BIN (w) ) ) ), 0));
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel,CPF_STYLE_FLAT);
    gtk_widget_set_size_request(button,17,17);
    g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (_lib_backgroundjobs_cancel_callback), (gpointer)job);
    gtk_box_pack_start (hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
    gtk_widget_show_all(button);
  }
  
  if(needlock) gdk_threads_leave();
}


static void _lib_backgroundjobs_progress(dt_lib_module_t *self, guint key, double progress)
{
  if(!darktable.control->running) return;
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;  
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, (gpointer)key);
  if(j)
  {
    /* check if progress is above 1.0 and destroy bgjob if finished */
    if (progress >= 1.0)
    {
      if (GTK_IS_WIDGET(j->widget))
	gtk_container_remove( GTK_CONTAINER(d->jobbox), j->widget );
      
      /* hide jobbox if theres no jobs left */
      if (g_list_length(gtk_container_get_children(GTK_CONTAINER(d->jobbox))) == 0 )
	gtk_widget_hide(d->jobbox);
    }
    else
    {
      if( j->type == 0 )
	gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR(j->progressbar), progress );
    }
  }

  if(needlock) gdk_threads_leave();
}

