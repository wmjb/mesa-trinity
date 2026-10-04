#ifndef GRATE_NIR_H
#define GRATE_NIR_H

struct nir_shader;

/* Bring a vertex shader into the shape the vertex unit can take: vec4 maths,
 * no control flow, io resolved to load_input/store_output. */
void grate_nir_lower_vs(struct nir_shader *s);

/* Bring a fragment shader into the shape the fragment unit can take: scalar
 * maths, no control flow, varyings resolved to load_input. */
void grate_nir_lower_fs(struct nir_shader *s);

#endif
