// typing.Annotated -- used to represent e.g. T @M
#include "Python.h"
#include "pycore_annotatedobject.h"
#include "pycore_interp.h"   // _PyInterpreterState_GET()
#include "pycore_object.h"  // _PyObject_GC_TRACK/UNTRACK
#include "pycore_tuple.h"
#include "pycore_typevarobject.h"  // _Py_typing_type_repr
#include "pycore_unionobject.h"
#include "pycore_unicodeobject.h"
#include "pycore_weakref.h"       // FT_CLEAR_WEAKREFS()


typedef struct {
    PyObject_HEAD
    PyObject *args;   // Tuple of (type, *metadata)
    PyObject *parameters; // Tuple of type variables or NULL
    PyObject *weakreflist;
} annotatedobject;

static int
annotated_clear(PyObject *self)
{
    annotatedobject *at = (annotatedobject *)self;
    Py_CLEAR(at->args);
    Py_CLEAR(at->parameters);
    return 0;
}

static void
annotated_dealloc(PyObject *self)
{
    annotatedobject *at = (annotatedobject *)self;

    _PyObject_GC_UNTRACK(self);
    FT_CLEAR_WEAKREFS(self, at->weakreflist);

    (void)annotated_clear(self);
    Py_TYPE(self)->tp_free(self);
}

static int
annotated_traverse(PyObject *self, visitproc visit, void *arg)
{
    annotatedobject *at = (annotatedobject *)self;
    Py_VISIT(at->args);
    Py_VISIT(at->parameters);
    return 0;
}

static Py_hash_t
annotated_hash(PyObject *self)
{
    annotatedobject *at = (annotatedobject *)self;
    return PyObject_Hash(at->args);
}

static PyObject *
annotated_richcompare(PyObject *self, PyObject *other, int op)
{
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    if (!_PyAnnotated_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    annotatedobject *at1 = (annotatedobject *)self;
    annotatedobject *at2 = (annotatedobject *)other;

    return PyObject_RichCompare(at1->args, at2->args, op);
}

static PyObject *
annotated_repr(PyObject *self)
{
    annotatedobject *at = (annotatedobject *)self;
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }

    PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
    // The `|` operator has a lower precedence than `@`, so we need to
    // add parentheses around a union.
    int needs_parens = _PyUnion_Check(origin);
    if (needs_parens && PyUnicodeWriter_WriteChar(writer, '(') < 0) {
        goto error;
    }

    if (_Py_typing_type_repr(writer, origin) < 0) {
        goto error;
    }

    if (needs_parens && PyUnicodeWriter_WriteChar(writer, ')') < 0) {
        goto error;
    }

    Py_ssize_t n = PyTuple_GET_SIZE(at->args);
    for (Py_ssize_t i = 1; i < n; i++) {
        if (PyUnicodeWriter_WriteASCII(writer, " @", 2) < 0) {
            goto error;
        }
        PyObject *item = PyTuple_GET_ITEM(at->args, i);
        if (PyUnicodeWriter_WriteRepr(writer, item) < 0) {
            goto error;
        }
    }

    return PyUnicodeWriter_Finish(writer);

error:
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static PyMemberDef annotated_members[] = {
    {"__args__", Py_T_OBJECT_EX, offsetof(annotatedobject, args), Py_READONLY},
    {0}
};

// Populate __parameters__ if needed.
static int
annotated_init_parameters(annotatedobject *at)
{
    return _Py_typing_init_parameters((PyObject *)at, &at->parameters, at->args);
}

/* Construct an Annotated object from an origin and a metadata tuple.
   Takes care of flattening if the origin is already an Annotated object.
*/
static PyObject *
make_annotated(PyObject *origin, PyObject *metadata)
{
    Py_ssize_t metadata_size = PyTuple_GET_SIZE(metadata);
    PyObject *args;
    if (_PyAnnotated_Check(origin)) {
        annotatedobject *at_origin = (annotatedobject *)origin;
        Py_ssize_t old_n = PyTuple_GET_SIZE(at_origin->args);
        args = PyTuple_New(old_n + metadata_size);
        if (args == NULL) {
            return NULL;
        }
        for (Py_ssize_t i = 0; i < old_n; i++) {
            PyTuple_SET_ITEM(args, i, Py_NewRef(PyTuple_GET_ITEM(at_origin->args, i)));
        }
        for (Py_ssize_t i = 0; i < metadata_size; i++) {
            PyTuple_SET_ITEM(args, old_n + i, Py_NewRef(PyTuple_GET_ITEM(metadata, i)));
        }
    }
    else {
        args = PyTuple_New(metadata_size + 1);
        if (args == NULL) {
            return NULL;
        }
        PyTuple_SET_ITEM(args, 0, Py_NewRef(origin));
        for (Py_ssize_t i = 0; i < metadata_size; i++) {
            PyTuple_SET_ITEM(args, i + 1, Py_NewRef(PyTuple_GET_ITEM(metadata, i)));
        }
    }

    annotatedobject *at = PyObject_GC_New(annotatedobject, &_PyAnnotated_Type);
    if (at == NULL) {
        Py_DECREF(args);
        return NULL;
    }

    at->args = args;
    at->parameters = NULL;
    at->weakreflist = NULL;

    _PyObject_GC_TRACK(at);
    return (PyObject *)at;
}

static PyObject *
annotated_getitem(PyObject *self, PyObject *item)
{
    annotatedobject *at = (annotatedobject *)self;
    if (annotated_init_parameters(at) < 0) {
        return NULL;
    }

    PyObject *newargs = _Py_subs_parameters(self, at->args, at->parameters, item);
    if (newargs == NULL) {
        return NULL;
    }

    PyObject *origin = Py_NewRef(PyTuple_GET_ITEM(newargs, 0));
    PyObject *metadata = PyTuple_GetSlice(newargs, 1, PyTuple_GET_SIZE(newargs));
    Py_DECREF(newargs);
    if (metadata == NULL) {
        Py_DECREF(origin);
        return NULL;
    }

    PyObject *res = make_annotated(origin, metadata);
    Py_DECREF(origin);
    Py_DECREF(metadata);
    return res;
}

static PyMappingMethods annotated_as_mapping = {
    .mp_subscript = annotated_getitem,
};

static PyObject *
annotated_origin(PyObject *self, void *Py_UNUSED(ignored))
{
    annotatedobject *at = (annotatedobject *)self;
    return Py_NewRef(PyTuple_GET_ITEM(at->args, 0));
}

static PyObject *
annotated_metadata(PyObject *self, void *Py_UNUSED(ignored))
{
    annotatedobject *at = (annotatedobject *)self;
    Py_ssize_t size = PyTuple_GET_SIZE(at->args);
    PyObject *metadata = PyTuple_New(size - 1);
    if (metadata == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 1; i < size; i++) {
        PyTuple_SET_ITEM(metadata, i - 1, Py_NewRef(PyTuple_GET_ITEM(at->args, i)));
    }
    return metadata;
}

static PyObject *
annotated_parameters(PyObject *self, void *Py_UNUSED(unused))
{
    annotatedobject *at = (annotatedobject *)self;
    if (annotated_init_parameters(at) < 0) {
        return NULL;
    }
    return Py_NewRef(at->parameters);
}

static PyObject *
annotated_name(PyObject *Py_UNUSED(self), void *Py_UNUSED(ignored))
{
    return PyUnicode_FromString("Annotated");
}

static PyObject *
annotated_module(PyObject *Py_UNUSED(self), void *Py_UNUSED(ignored))
{
    return PyUnicode_FromString("typing");
}

static PyGetSetDef annotated_properties[] = {
    {"__name__", annotated_name, NULL,
     PyDoc_STR("Name of the type"), NULL},
    {"__qualname__", annotated_name, NULL,
     PyDoc_STR("Qualified name of the type"), NULL},
    {"__module__", annotated_module, NULL,
     PyDoc_STR("Module of the type"), NULL},
    {"__origin__", annotated_origin, NULL,
     PyDoc_STR("The origin of the annotated type"), NULL},
    {"__metadata__", annotated_metadata, NULL,
     PyDoc_STR("Metadata of the annotated type"), NULL},
    {"__parameters__", annotated_parameters, NULL,
     PyDoc_STR("Type variables in the types.AnnotatedType."), NULL},
    {0}
};

static PyObject *
annotated_matmul(PyObject *self, PyObject *other)
{
    PyObject *metadata = PyTuple_Pack(1, other);
    if (metadata == NULL) {
        return NULL;
    }
    PyObject *res = make_annotated(self, metadata);
    Py_DECREF(metadata);
    return res;
}

static PyNumberMethods annotated_as_number = {
    .nb_matrix_multiply = annotated_matmul,
    .nb_or = _Py_union_type_or,
};

static const char* const attr_exceptions[] = {
    "__class__",
    "__name__",
    "__qualname__",
    "__module__",
    "__origin__",
    "__args__",
    "__metadata__",
    "__parameters__",
    "__typing_subst__",
    "__typing_unpacked_tuple_args__",
    "__mro_entries__",
    "__reduce_ex__",
    "__reduce__",
    NULL,
};

static const char* const attr_blocked[] = {
    "__bases__",
    "__copy__",
    "__deepcopy__",
    NULL,
};

static PyObject *
annotated_getattro(PyObject *self, PyObject *name)
{
    annotatedobject *at = (annotatedobject *)self;
    PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
    return _Py_typing_proxy_getattro(self, origin, name, attr_blocked, attr_exceptions);
}

static int
annotated_setattro(PyObject *self, PyObject *name, PyObject *value)
{
    annotatedobject *at = (annotatedobject *)self;
    if (PyUnicode_Check(name)) {
        for (const char * const *p = attr_exceptions; ; p++) {
            if (*p == NULL) {
                PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
                return PyObject_SetAttr(origin, name, value);
            }
            if (_PyUnicode_EqualToASCIIString(name, *p)) {
                break;
            }
        }
    }
    return PyObject_GenericSetAttr(self, name, value);
}

static PyObject *
annotated_call(PyObject *self, PyObject *args, PyObject *kwargs)
{
    annotatedobject *at = (annotatedobject *)self;
    PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
    PyObject *obj = PyObject_Call(origin, args, kwargs);
    return _Py_typing_set_orig_class(obj, self);
}

static PyObject *
annotated_class_getitem(PyObject *cls, PyObject *args)
{
    if (!PyTuple_Check(args) || PyTuple_GET_SIZE(args) < 2) {
        PyErr_SetString(PyExc_TypeError,
                        "Annotated[...] requires at least two arguments");
        return NULL;
    }

    PyObject *origin = PyTuple_GET_ITEM(args, 0);
    PyObject *checked_origin = _Py_typing_type_check(origin, "Annotated[t, ...]: t must be a type.", 1);
    if (checked_origin == NULL) {
        return NULL;
    }


    if (_Py_is_unpacked_typevartuple(checked_origin)) {
        Py_DECREF(checked_origin);
        PyErr_SetString(PyExc_TypeError,
                        "Annotated[...] should not be used with an "
                        "unpacked TypeVarTuple");
        return NULL;
    }

    PyObject *metadata = PyTuple_GetSlice(args, 1, PyTuple_GET_SIZE(args));
    if (metadata == NULL) {
        Py_DECREF(checked_origin);
        return NULL;
    }

    PyObject *res = make_annotated(checked_origin, metadata);
    Py_DECREF(checked_origin);
    Py_DECREF(metadata);
    return res;
}

static PyObject *
annotated_instancecheck(PyObject *self, PyObject *obj)
{
    PyErr_SetString(PyExc_TypeError,
                    "isinstance() argument 2 cannot be an Annotated type");
    return NULL;
}

static PyObject *
annotated_subclasscheck(PyObject *self, PyObject *cls)
{
    PyErr_SetString(PyExc_TypeError,
                    "issubclass() argument 2 cannot be an Annotated type");
    return NULL;
}

static PyObject *
annotated_mro_entries(PyObject *self, PyObject *args)
{
    annotatedobject *at = (annotatedobject *)self;
    PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
    return PyTuple_Pack(1, origin);
}

static PyObject *
annotated_dir(PyObject *self, PyObject *Py_UNUSED(unused))
{
    annotatedobject *at = (annotatedobject *)self;
    PyObject *origin = PyTuple_GET_ITEM(at->args, 0);
    return _Py_typing_proxy_dir(origin, attr_exceptions);
}

static PyMethodDef annotated_methods[] = {
    {"__mro_entries__", annotated_mro_entries, METH_O},
    {"__class_getitem__", annotated_class_getitem, METH_O|METH_CLASS, PyDoc_STR("See PEP 585")},
    {"__instancecheck__", annotated_instancecheck, METH_O},
    {"__subclasscheck__", annotated_subclasscheck, METH_O},
    {"__dir__", annotated_dir, METH_NOARGS},
    {0}
};

PyTypeObject _PyAnnotated_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "typing.Annotated",
    .tp_doc = PyDoc_STR("Represent an annotated type.\n"
              "\n"
              "E.g. for int @M"),
    .tp_basicsize = sizeof(annotatedobject),
    .tp_dealloc = annotated_dealloc,
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_GC_Del,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = annotated_traverse,
    .tp_clear = annotated_clear,
    .tp_hash = annotated_hash,
    .tp_getattro = annotated_getattro,
    .tp_setattro = annotated_setattro,
    .tp_call = annotated_call,
    .tp_members = annotated_members,
    .tp_methods = annotated_methods,
    .tp_richcompare = annotated_richcompare,
    .tp_as_mapping = &annotated_as_mapping,
    .tp_as_number = &annotated_as_number,
    .tp_repr = annotated_repr,
    .tp_getset = annotated_properties,
    .tp_weaklistoffset = offsetof(annotatedobject, weakreflist),
};

PyObject *
_PyAnnotated_New(PyObject *origin, PyObject *metadata)
{
    return make_annotated(origin, metadata);
}

PyObject *
_PyAnnotated_Matmul(PyObject *self, PyObject *other)
{
    if (!_Py_typing_is_type_like(self)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyObject *metadata = PyTuple_Pack(1, other);
    if (metadata == NULL) {
        return NULL;
    }
    PyObject *origin = self;
    PyObject *res = make_annotated(origin, metadata);
    Py_DECREF(metadata);
    return res;
}
