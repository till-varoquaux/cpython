#ifndef Py_INTERNAL_ANNOTATEDOBJECT_H
#define Py_INTERNAL_ANNOTATEDOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "Python.h"

extern PyTypeObject _PyAnnotated_Type;

#define _PyAnnotated_Check(op) (Py_IS_TYPE(op, &_PyAnnotated_Type))

/* Create a new AnnotatedType object.
   origin: The base type being annotated.
   metadata: A tuple of metadata.
*/
PyObject *_PyAnnotated_New(PyObject *origin, PyObject *metadata);

/* Implement v @w. Returns an AnnotatedType or NotImplemented. */
PyObject *_PyAnnotated_Matmul(PyObject *v, PyObject *w);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_ANNOTATEDOBJECT_H */
