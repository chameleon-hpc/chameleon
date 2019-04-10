module chameleon_lib
 use iso_c_binding
 implicit none

type, bind(c) :: map_entry 
  type(c_ptr) :: valptr
  integer(kind=c_size_t) :: size
  integer(kind=c_int)    :: type
end type map_entry

public:: map_entry

interface
 function chameleon_init() bind(c)
   implicit none
   integer :: chameleon_init
 end function chameleon_init

 function chameleon_determine_base_addresses(func) BIND(c) 
   use iso_c_binding
   implicit none
   type(c_ptr), intent(in) :: func
   integer:: chameleon_determine_base_addresses
   
 end function chameleon_determine_base_addresses

 type(c_ptr) function chameleon_create_annotation_container_fortran() BIND(c) 
    use iso_c_binding
    implicit none
  end function chameleon_create_annotation_container_fortran

  function chameleon_set_annotation_int_fortran(ann, value) BIND(c)
    use iso_c_binding
    implicit none
    type(c_ptr), value, intent(in) :: ann
    integer(kind=c_int), value, intent(in) :: value
    integer :: chameleon_set_annotation_int_fortran
  end function chameleon_set_annotation_int_fortran

  function chameleon_get_annotation_int_fortran(ann) BIND(c)
    use iso_c_binding
    implicit none
    type(c_ptr), value, intent(in) :: ann
    integer :: chameleon_get_annotation_int_fortran
  end function chameleon_get_annotation_int_fortran

 function chameleon_add_task_manual_fortran(entry, nargs, args) bind(c, name="chameleon_add_task_manual_fortran")
   use iso_c_binding
   implicit none
   type(c_funptr), value, intent(in) :: entry
   integer(kind=c_int), value, intent(in) :: nargs
   type(c_ptr), intent(in), value :: args
   integer:: chameleon_add_task_manual_fortran
 end function chameleon_add_task_manual_fortran

 function chameleon_add_task_manual_fortran_w_annotations(entry, nargs, args, annotations) bind(c, name="chameleon_add_task_manual_fortran_w_annotations")
    use iso_c_binding
    implicit none
    type(c_funptr), value, intent(in) :: entry
    integer(kind=c_int), value, intent(in) :: nargs
    type(c_ptr), intent(in), value :: args
    !type(map_entry),dimension(:) :: args
    type(c_ptr), intent(in), value :: annotations
    integer:: chameleon_add_task_manual_fortran_w_annotations
  end function chameleon_add_task_manual_fortran_w_annotations

 function chameleon_distributed_taskwait(nowait) bind(c)
   use iso_c_binding
   implicit none
   integer(kind=c_int), intent(in) :: nowait
 
   integer :: chameleon_distributed_taskwait
 end function chameleon_distributed_taskwait

 function chameleon_finalize() bind(c)
   implicit none
   integer :: chameleon_finalize
 end function chameleon_finalize
end interface

contains
 function chameleon_add_task_manual(entry, nargs, args)
   use iso_c_binding
   implicit none
   procedure(),pointer, intent(in) :: entry
   integer(kind=c_int) :: nargs
   type(map_entry),dimension(:) :: args
   integer(kind=c_int) :: chameleon_add_task_manual
   
   chameleon_add_task_manual = chameleon_add_task_manual_fortran(c_funloc(entry), nargs, c_loc(args))
 end function

end module


