#ifndef QDEV_CORE_H
#define QDEV_CORE_H

#include "qemu/queue.h"
#include "qemu/option.h"
#include "qemu/bitmap.h"
#include "qom/object.h"
#include "hw/irq.h"
#include "hw/hotplug.h"

enum {
    DEV_NVECTORS_UNSPECIFIED = -1,
};

#define TYPE_DEVICE "device"
#define DEVICE(obj) OBJECT_CHECK(DeviceState, (obj), TYPE_DEVICE)
#define DEVICE_CLASS(klass) OBJECT_CLASS_CHECK(DeviceClass, (klass), TYPE_DEVICE)
#define DEVICE_GET_CLASS(obj) OBJECT_GET_CLASS(DeviceClass, (obj), TYPE_DEVICE)

typedef enum DeviceCategory {
    DEVICE_CATEGORY_BRIDGE,
    DEVICE_CATEGORY_USB,
    DEVICE_CATEGORY_STORAGE,
    DEVICE_CATEGORY_NETWORK,
    DEVICE_CATEGORY_INPUT,
    DEVICE_CATEGORY_DISPLAY,
    DEVICE_CATEGORY_SOUND,
    DEVICE_CATEGORY_MISC,
    DEVICE_CATEGORY_MAX
} DeviceCategory;

typedef int (*qdev_initfn)(DeviceState *dev);
typedef int (*qdev_event)(DeviceState *dev);
typedef void (*qdev_resetfn)(DeviceState *dev);
typedef void (*DeviceRealize)(DeviceState *dev, Error **errp);
typedef void (*DeviceUnrealize)(DeviceState *dev, Error **errp);
typedef void (*BusRealize)(BusState *bus, Error **errp);
typedef void (*BusUnrealize)(BusState *bus, Error **errp);

struct VMStateDescription;

/**
 * DeviceClass:
 * @props: Properties accessing state fields.
 * @realize: Callback function invoked when the #DeviceState:realized
 * property is changed to %true. The default invokes @init if not %NULL.
 * @unrealize: Callback function invoked when the #DeviceState:realized
 * property is changed to %false.
 * @init: Callback function invoked when the #DeviceState::realized property
 * is changed to %true. Deprecated, new types inheriting directly from
 * TYPE_DEVICE should use @realize instead, new leaf types should consult
 * their respective parent type.
 * @hotpluggable: indicates if #DeviceClass is hotpluggable, available
 * as readonly "hotpluggable" property of #DeviceState instance
 *
 * # Realization #
 * Devices are constructed in two stages,
 * 1) object instantiation via object_initialize() and
 * 2) device realization via #DeviceState:realized property.
 * The former may not fail (it might assert or exit), the latter may return
 * error information to the caller and must be re-entrant.
 * Trivial field initializations should go into #TypeInfo.instance_init.
 * Operations depending on @props static properties should go into @realize.
 * After successful realization, setting static properties will fail.
 *
 * As an interim step, the #DeviceState:realized property can also be
 * set with qdev_init_nofail().
 * In the future, devices will propagate this state change to their children
 * and along busses they expose.
 * The point in time will be deferred to machine creation, so that values
 * set in @realize will not be introspectable beforehand. Therefore devices
 * must not create children during @realize; they should initialize them via
 * object_initialize() in their own #TypeInfo.instance_init and forward the
 * realization events appropriately.
 *
 * The @init callback is considered private to a particular bus implementation
 * (immediate abstract child types of TYPE_DEVICE). Derived leaf types set an
 * "init" callback on their parent class instead.
 *
 * Any type may override the @realize and/or @unrealize callbacks but needs
 * to call the parent type's implementation if keeping their functionality
 * is desired. Refer to QOM documentation for further discussion and examples.
 *
 * <note>
 *   <para>
 * If a type derived directly from TYPE_DEVICE implements @realize, it does
 * not need to implement @init and therefore does not need to store and call
 * #DeviceClass' default @realize callback.
 * For other types consult the documentation and implementation of the
 * respective parent types.
 *   </para>
 * </note>
 *
 * 
 * 所有设备类的积累
 */
typedef struct DeviceClass {
    /*< private >*/
    ObjectClass parent_class;
    /*< public >*/

    /*
     * 设备的种类,
     * DEVICE_CATEGORY_USB,
     * DEVICE_CATEGORY_NETWORK,
     * DEVICE_CATEGORY_BRIDGE,
     * DEVICE_CATEGORY_STORAGE,
     * DEVICE_CATEGORY_DISPLAY....
     */
    DECLARE_BITMAP(categories, DEVICE_CATEGORY_MAX);
	/*
	 * 通常用于生产设备在固件中的路径
	 */
    const char *fw_name;
	/*
	 * 用于描述设备
	 */
    const char *desc;
	//表示设备的属性
    Property *props;

    /*
     * Shall we hide this device model from -device / device_add?
     * All devices should support instantiation with device_add, and
     * this flag should not exist.  But we're not there, yet.  Some
     * devices fail to instantiate with cryptic error messages.
     * Others instantiate, but don't work.  Exposing users to such
     * behavior would be cruel; this flag serves to protect them.  It
     * should never be set without a comment explaining why it is set.
     * TODO remove once we're there
     */
    bool cannot_instantiate_with_device_add_yet;
    /*
     * Does this device model survive object_unref(object_new(TNAME))?
     * All device models should, and this flag shouldn't exist.  Some
     * devices crash in object_new(), some crash or hang in
     * object_unref().  Makes introspecting properties with
     * qmp_device_list_properties() dangerous.  Bad, because it's used
     * by -device FOO,help.  This flag serves to protect that code.
     * It should never be set without a comment explaining why it is
     * set.
     * TODO remove once we're there
     */
    bool cannot_destroy_with_object_finalize_yet;

    /*
     * 设备是否能进行hot plug
     */
    bool hotpluggable;

    /* callbacks 
     * 重置设备的状态
	 */
    void (*reset)(DeviceState *dev);
	/*
	 * 通常是device_realize
	 * 在pci_device_class_init中改为pci_qdev_realize,
	 * VirtioPCIClass在virtio_pci_class_init中设置这个realize=virtio_pci_realize
	 *
	 * DeviceClass子类有其自己的realize成员，比如VirtioDeviceClass->realize
	 *
	 * VirtioPCIClass时, pci_qdev_realize
	 */
    DeviceRealize realize;
    DeviceUnrealize unrealize;

    /* device state 
     * 表示设备的状态，在虚拟机migrate的时候,需要记录设备的状态
     * 以便在目的主机上恢复虚拟机
	 */
    const struct VMStateDescription *vmsd;

    /* Private to qdev / bus.  */
    qdev_initfn init; /* TODO remove, once users are converted to realize */
    qdev_event exit; /* TODO remove, once users are converted to unrealize */
	//该设备挂载的总线类型
    const char *bus_type;
} DeviceClass;

typedef struct NamedGPIOList NamedGPIOList;

struct NamedGPIOList {
    char *name;
    qemu_irq *in;
    int num_in;
    int num_out;
    QLIST_ENTRY(NamedGPIOList) node;
};

/**
 * DeviceState:
 * @realized: Indicates whether the device has been fully constructed.
 *
 * This structure should not be accessed directly.  We declare it here
 * so that it can be embedded in individual device state structures.
 *
 *
 */
struct DeviceState {
    /*< private >*/
    Object parent_obj;
    /*< public >*/

    /*
     * 设备名称
     */
    const char *id;
	/*
	 * 是否已经具现化
	 */
    bool realized;
	/*
	 * 设备实例销毁的时候判断是否已经具现化，如果已经具现化,就发送一个DEVICE_DELETED事件
	 */
    bool pending_deleted_event;
	//设备对应的参数
    QemuOpts *opts;
	//设备是否是通过热插拔进入到系统的
    int hotplugged;
	//设备所挂载在的总线
    BusState *parent_bus;
	/*
	 * 表示General purpose input/output,相当于设备的输入和输出,用来模拟硬件的pin
	 */
    QLIST_HEAD(, NamedGPIOList) gpios;
	/* 
	 * 链接该设备下所有的总线
     */
    QLIST_HEAD(, BusState) child_bus;
	//总线的序号
    int num_child_bus;
	//migrate的时候保存当前虚拟机的实例的编号
    int instance_id_alias;
	//migrate时,只有当这个设备的这个值大于或等于设备VMStateDescription->minimum_version_id
    int alias_required_for_version;
};

struct DeviceListener {
    void (*realize)(DeviceListener *listener, DeviceState *dev);
    void (*unrealize)(DeviceListener *listener, DeviceState *dev);
    QTAILQ_ENTRY(DeviceListener) link;
};

#define TYPE_BUS "bus"
#define BUS(obj) OBJECT_CHECK(BusState, (obj), TYPE_BUS)
#define BUS_CLASS(klass) OBJECT_CLASS_CHECK(BusClass, (klass), TYPE_BUS)
#define BUS_GET_CLASS(obj) OBJECT_GET_CLASS(BusClass, (obj), TYPE_BUS)

/*
 * 所有总线的基类
 * 对应的对象为BusState
 */
struct BusClass {
    ObjectClass parent_class;

    /* FIXME first arg should be BusState */
	/*
	 * 打印总线上的一个设备
	 */
    void (*print_dev)(Monitor *mon, DeviceState *dev, int indent);
    char *(*get_dev_path)(DeviceState *dev);
    /*
     * This callback is used to create Open Firmware device path in accordance
     * with OF spec http://forthworks.com/standards/of1275.pdf. Individual bus
     * bindings can be found at http://playground.sun.com/1275/bindings/.
     *
     * 得到设备路径以及在firmware中的路径
     */
    char *(*get_fw_dev_path)(DeviceState *dev);
	/*
	 * 是表示Bus进行realize的回调函数
	 */
    void (*reset)(BusState *bus);
    BusRealize realize;
    BusUnrealize unrealize;

    /* maximum devices allowed on the bus, 0: no limit. 
     * 该bus上允许的最大设备
	 */
    int max_dev;
    /* number of automatically allocated bus ids (e.g. ide.0)
     * 自动生成的bus id序列号
     */
    int automatic_ids;
};

typedef struct BusChild {
    DeviceState *child;
    int index;
    QTAILQ_ENTRY(BusChild) sibling;
} BusChild;

#define QDEV_HOTPLUG_HANDLER_PROPERTY "hotplug-handler"

/**
 * BusState:
 * @hotplug_device: link to a hotplug device associated with bus.
 */
struct BusState {
 
    Object obj;
	/*
	 * 表示总线所在的设备
	 */
    DeviceState *parent;
    char *name;
	/*
	 * 处理热插拔
	 */
    HotplugHandler *hotplug_handler;
	/*
	 * 表示该总线最大的设备数量
	 */
    int max_index;
    bool realized;
	
    QTAILQ_HEAD(ChildrenHead, BusChild) children;
    /*
     * 链接在一条总线上的设备
     */
	QLIST_ENTRY(BusState) sibling;
};

struct Property {
    const char   *name;
    PropertyInfo *info;
    ptrdiff_t    offset;
    uint8_t      bitnr;
    QType        qtype;
    int64_t      defval;
    int          arrayoffset;
    PropertyInfo *arrayinfo;
    int          arrayfieldsize;
};

struct PropertyInfo {
    const char *name;
    const char *description;
    const char * const *enum_table;
    int (*print)(DeviceState *dev, Property *prop, char *dest, size_t len);
    ObjectPropertyAccessor *get;
    ObjectPropertyAccessor *set;
    ObjectPropertyRelease *release;
};

/**
 * GlobalProperty:
 * @user_provided: Set to true if property comes from user-provided config
 * (command-line or config file).
 * @used: Set to true if property was used when initializing a device.
 * @errp: Error destination, used like first argument of error_setg()
 *        in case property setting fails later. If @errp is NULL, we
 *        print warnings instead of ignoring errors silently. For
 *        hotplugged devices, errp is always ignored and warnings are
 *        printed instead.
 */
typedef struct GlobalProperty {
    const char *driver;
    const char *property;
    const char *value;
    bool user_provided;
    bool used;
    Error **errp;
} GlobalProperty;

/*** Board API.  This should go away once we have a machine config file.  ***/

DeviceState *qdev_create(BusState *bus, const char *name);
DeviceState *qdev_try_create(BusState *bus, const char *name);
void qdev_init_nofail(DeviceState *dev);
void qdev_set_legacy_instance_id(DeviceState *dev, int alias_id,
                                 int required_for_version);
HotplugHandler *qdev_get_hotplug_handler(DeviceState *dev);
void qdev_unplug(DeviceState *dev, Error **errp);
void qdev_simple_device_unplug_cb(HotplugHandler *hotplug_dev,
                                  DeviceState *dev, Error **errp);
void qdev_machine_creation_done(void);
bool qdev_machine_modified(void);

qemu_irq qdev_get_gpio_in(DeviceState *dev, int n);
qemu_irq qdev_get_gpio_in_named(DeviceState *dev, const char *name, int n);

void qdev_connect_gpio_out(DeviceState *dev, int n, qemu_irq pin);
void qdev_connect_gpio_out_named(DeviceState *dev, const char *name, int n,
                                 qemu_irq pin);
qemu_irq qdev_get_gpio_out_connector(DeviceState *dev, const char *name, int n);
qemu_irq qdev_intercept_gpio_out(DeviceState *dev, qemu_irq icpt,
                                 const char *name, int n);

BusState *qdev_get_child_bus(DeviceState *dev, const char *name);

/*** Device API.  ***/

/* Register device properties.  */
/* GPIO inputs also double as IRQ sinks.  */
void qdev_init_gpio_in(DeviceState *dev, qemu_irq_handler handler, int n);
void qdev_init_gpio_out(DeviceState *dev, qemu_irq *pins, int n);
void qdev_init_gpio_in_named(DeviceState *dev, qemu_irq_handler handler,
                             const char *name, int n);
void qdev_init_gpio_out_named(DeviceState *dev, qemu_irq *pins,
                              const char *name, int n);

void qdev_pass_gpios(DeviceState *dev, DeviceState *container,
                     const char *name);

BusState *qdev_get_parent_bus(DeviceState *dev);

/*** BUS API. ***/

DeviceState *qdev_find_recursive(BusState *bus, const char *id);

/* Returns 0 to walk children, > 0 to skip walk, < 0 to terminate walk. */
typedef int (qbus_walkerfn)(BusState *bus, void *opaque);
typedef int (qdev_walkerfn)(DeviceState *dev, void *opaque);

void qbus_create_inplace(void *bus, size_t size, const char *typename,
                         DeviceState *parent, const char *name);
BusState *qbus_create(const char *typename, DeviceState *parent, const char *name);
/* Returns > 0 if either devfn or busfn skip walk somewhere in cursion,
 *         < 0 if either devfn or busfn terminate walk somewhere in cursion,
 *           0 otherwise. */
int qbus_walk_children(BusState *bus,
                       qdev_walkerfn *pre_devfn, qbus_walkerfn *pre_busfn,
                       qdev_walkerfn *post_devfn, qbus_walkerfn *post_busfn,
                       void *opaque);
int qdev_walk_children(DeviceState *dev,
                       qdev_walkerfn *pre_devfn, qbus_walkerfn *pre_busfn,
                       qdev_walkerfn *post_devfn, qbus_walkerfn *post_busfn,
                       void *opaque);

void qdev_reset_all(DeviceState *dev);
void qdev_reset_all_fn(void *opaque);

/**
 * @qbus_reset_all:
 * @bus: Bus to be reset.
 *
 * Reset @bus and perform a bus-level ("hard") reset of all devices connected
 * to it, including recursive processing of all buses below @bus itself.  A
 * hard reset means that qbus_reset_all will reset all state of the device.
 * For PCI devices, for example, this will include the base address registers
 * or configuration space.
 */
void qbus_reset_all(BusState *bus);
void qbus_reset_all_fn(void *opaque);

/* This should go away once we get rid of the NULL bus hack */
BusState *sysbus_get_default(void);

char *qdev_get_fw_dev_path(DeviceState *dev);
char *qdev_get_own_fw_dev_path_from_handler(BusState *bus, DeviceState *dev);

/**
 * @qdev_machine_init
 *
 * Initialize platform devices before machine init.  This is a hack until full
 * support for composition is added.
 */
void qdev_machine_init(void);

/**
 * @device_reset
 *
 * Reset a single device (by calling the reset method).
 */
void device_reset(DeviceState *dev);

const struct VMStateDescription *qdev_get_vmsd(DeviceState *dev);

const char *qdev_fw_name(DeviceState *dev);

Object *qdev_get_machine(void);

/* FIXME: make this a link<> */
void qdev_set_parent_bus(DeviceState *dev, BusState *bus);

extern int qdev_hotplug;

char *qdev_get_dev_path(DeviceState *dev);

GSList *qdev_build_hotpluggable_device_list(Object *peripheral);

void qbus_set_hotplug_handler(BusState *bus, DeviceState *handler,
                              Error **errp);

void qbus_set_bus_hotplug_handler(BusState *bus, Error **errp);

static inline bool qbus_is_hotpluggable(BusState *bus)
{
   return bus->hotplug_handler;
}

void device_listener_register(DeviceListener *listener);
void device_listener_unregister(DeviceListener *listener);

#endif
