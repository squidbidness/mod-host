/*
 * This file is part of mod-host.
 *
 * mod-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mod-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mod-host.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <inttypes.h>
#include <stdio.h>
#include <lv2/lv2plug.in/ns/ext/atom/util.h>

#include "atom-writer.h"


/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL CONSTANTS
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL MACROS
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL FUNCTION PROTOTYPES
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL CONFIGURATION ERRORS
************************************************************************************************************************
*/


/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

static void sink(AtomWriter* writer, const void* buf, size_t len)
{
    if (writer->overflow || len == 0) {
        return;
    }
    if (writer->len + len >= sizeof(writer->buf)) {
        writer->overflow = true;
        return;
    }
    memcpy(&writer->buf[writer->len], buf, len);
    writer->len += len;
    writer->buf [writer->len] = 0;
}

static void sink_string(AtomWriter* w, const char* str)
{
    sink(w, str, strlen(str));
}

/* escape double-quotes (") and backslashes (\)
 * TODO also escape ctrl chars, see http://json.org/
 */
static void sink_escape(AtomWriter* w, const char* str)
{
    assert(str);
    sink(w, "\"", 1);
    const char* pos = str;
    do {
        size_t off = strcspn(pos, "\"\\");
        sink(w, pos, off);
        if (off == strlen(pos)) {
            break;
        }
        sink(w, "\\", 1);
        sink(w, &pos[off], 1);
        pos = &pos[++off];
    } while (*pos);
    sink(w, "\"", 1);
}

static void sink_begin_object(AtomWriter* w, const char* name)
{
    sink(w, "{", 1);
    sink_escape(w, name);
    sink(w, ":", 1);
}

static void sink_end_object(AtomWriter* w)
{
    sink(w, "}", 1);
}

#define SSTR(str) ((const char*)(str))

// compare to sratom_write() -- this writes JSON
//
// ideally serd would support SERD_JSON syntax output and sratom would
// allow to use a format to anything other than SERD_TURTLE.
// and https://dvcs.w3.org/hg/rdf/raw-file/default/rdf-json/index.html should produces less
// verbose output easier to be handled by MOD-UIs :)
//
// XXX the format is pretty much ad-hoc.
//
// JS is dynamically typed, so the produced format is not reversible back
// into an Atom. float/double int32/64 string/URI/URID/bool are ambiguous
// without extra type info.
//
// However this data is passed via ringbuffer out of RT-context, then
// via socket to the mod-ui and finally via websocket to a browser GUI.
// Explicit XSD "type" URIs and "value" keys-pairs may be significant overhead
// and also complicate parsing in the MOD JS GUI.

static void serialize_atom(AtomWriter* writer, uint32_t type_urid, uint32_t size, const uint8_t* body)
{
    if (type_urid == 0 && size == 0) {
        sink_string(writer, "null");
    } else if (type_urid == writer->forge.String) {
        sink_escape(writer, SSTR(body));
    } else if (type_urid == writer->forge.URID) {
        const uint32_t urid = *(const uint32_t*)body;
        sink_escape(writer, writer->unmap->unmap(writer->unmap->handle, urid));
    } else if (type_urid == writer->forge.URI) {
        sink_escape(writer, SSTR(body));
    } else if (type_urid == writer->forge.Int) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%" PRId32, *(const int32_t*)body);
        sink_string(writer, buf);
    } else if (type_urid == writer->forge.Long) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%" PRId64, *(const int64_t*)body);
        sink_string(writer, buf);
    } else if (type_urid == writer->forge.Float) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%f", *(const float*)body);
        sink_string(writer, buf);
    } else if (type_urid == writer->forge.Double) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%f", *(const double*)body);
        sink_string(writer, buf);
    } else if (type_urid == writer->forge.Bool) {
        const int32_t val = *(const int32_t*)body;
        sink_string(writer, SSTR(val ? "true" : "false"));
    } else if (type_urid == writer->forge.Vector) {
        const LV2_Atom_Vector_Body* vec = (const LV2_Atom_Vector_Body*)body;
        bool first = true;
        sink(writer, "[", 1);
        for (const uint8_t* i = (const uint8_t*)(vec + 1); i < (const uint8_t*)vec + size; i += vec->child_size)
        {
            if (first) {
                first = false;
            } else {
                sink(writer, ",", 1);
            }
            serialize_atom(writer, vec->child_type, vec->child_size, i);
        }
        sink(writer, "]", 1);
    } else if (lv2_atom_forge_is_object_type(&writer->forge, type_urid)) {
        const LV2_Atom_Object_Body* obj = (const LV2_Atom_Object_Body*)body;
        const char* otype = writer->unmap->unmap(writer->unmap->handle, obj->otype);

        sink_begin_object(writer, otype); // key, subject
        sink(writer, "{", 1); // value, object

        if (lv2_atom_forge_is_blank(&writer->forge, type_urid, obj)) {
            sink_string(writer, "\"#type\":null");
        } else {
            sink_string(writer, "\"#type\":");
            sink_escape(writer, writer->unmap->unmap(writer->unmap->handle, obj->id));
        }

        LV2_ATOM_OBJECT_BODY_FOREACH(obj, size, prop)
        {
            const char* const key  = writer->unmap->unmap(writer->unmap->handle, prop->key);
            sink(writer, ",", 1);
            sink_escape(writer, key);
            sink(writer, ":", 1);
            serialize_atom(writer, prop->value.type, prop->value.size, LV2_ATOM_BODY(&prop->value));
        }

        sink(writer, "}", 1);
        sink_end_object(writer);
    } else if (type_urid == writer->forge.Path) {
        // TODO check is path absolute and/or valid URI (file://)
        sink_begin_object(writer, "#path");
        sink_escape(writer, SSTR(body));
        sink_end_object(writer);
    } else {
        // TODO handle more Atom types:
        // - midi_MidiEvent
        // - atom_Event (timestamp, beat/frame time) -- to gui??
        // - writer->forge.Sequence -- to gui? (top-level parses sequence into separate events)
        // - writer->forge.Tuple
        // - writer->forge.Literal (w/ language)
        // - writer->forge.Chunk (base64Binary)
        // - fallback: blob, base64
        sink_string(writer, "null");
    }
}


/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

void atom_writer_init(AtomWriter* w, LV2_Atom_Forge* forge, LV2_URID_Unmap* unmap)
{
    w->unmap = unmap;
    w->forge = *forge;
    w->overflow = false;
    w->len = 0;
    w->buf[0] = 0;

    lv2_atom_forge_set_buffer(&w->forge, w->forgeBuf, sizeof(w->forgeBuf));
}

int atom_writer_format(AtomWriter* w, int instance, const char *symbol, uint32_t type_urid, uint32_t size, uint8_t* body)
{
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "atom %d %s ", instance, symbol);
    hdr[255] = 0;
    sink_string(w, hdr);

    // XXX the port-symbol here is redundant, but we need some top-level
    // to produce valid JSON. -> mod-ui's host.js could fix that.
    sink_begin_object(w, symbol);
    serialize_atom(w, type_urid, size, body);
    sink_end_object(w);
    sink(w, "\n", 1);
    return w->overflow ? -1 : 0;
}
